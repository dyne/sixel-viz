#include "terminal_playback.hpp"

#include "fit.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#endif

namespace si {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using namespace std::chrono_literals;

constexpr auto enter_screen = "\033[?1049h\033[?25l";
constexpr auto leave_screen = "\033[?1049l\033[?25h";
constexpr auto cancel_picture = "\030\033\\";
constexpr auto home = "\033[1;1H";
constexpr auto clear_screen = "\033[2J";
constexpr std::size_t write_chunk = 4096;

int timeout_ms(TimePoint deadline) {
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
  return static_cast<int>(std::clamp(remaining.count(), decltype(remaining.count()){0},
      static_cast<decltype(remaining.count())>(std::numeric_limits<int>::max())));
}

#ifdef _WIN32

[[noreturn]] void windows_error(DWORD error, const char* operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

SRWLOCK control_lock = SRWLOCK_INIT;
HANDLE control_event = nullptr;
HANDLE writing_thread = nullptr;

BOOL WINAPI wake_on_control(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  HANDLE signaled_event = nullptr;
  HANDLE writer = nullptr;
  AcquireSRWLockShared(&control_lock);
  if (control_event && SetEvent(control_event)) {
    signaled_event = control_event;
    writer = writing_thread;
  }
  ReleaseSRWLockShared(&control_lock);
  if (!signaled_event) return FALSE;

  if (writer) {
    detail::retry_console_cancellation([&] {
      AcquireSRWLockShared(&control_lock);
      // The writer can pass its event check before WriteFile has pending I/O.
      // Recheck registration under the lock so cleanup writes stay outside it.
      const bool retry = control_event == signaled_event && writing_thread == writer &&
          !CancelSynchronousIo(writer) && GetLastError() == ERROR_NOT_FOUND;
      ReleaseSRWLockShared(&control_lock);
      return retry;
    }, [] { Sleep(1); });
  }
  return TRUE;
}

class TerminalDevice {
public:
  TerminalDevice() {
    const auto descriptor = _fileno(stdout);
    if (descriptor < 0) throw std::runtime_error("could not get console stdout");
    output_ = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    if (!GetConsoleMode(output_, &mode_)) windows_error(GetLastError(), "read console output mode");
    try {
      event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!event_) windows_error(GetLastError(), "create playback interrupt event");
      const auto process = GetCurrentProcess();
      if (!DuplicateHandle(process, GetCurrentThread(), process, &thread_, THREAD_TERMINATE,
                           FALSE, 0)) {
        windows_error(GetLastError(), "get playback thread handle");
      }
      AcquireSRWLockExclusive(&control_lock);
      control_event = event_;
      ReleaseSRWLockExclusive(&control_lock);
      if (!SetConsoleCtrlHandler(wake_on_control, TRUE)) {
        windows_error(GetLastError(), "install console interrupt handler");
      }
      handler_installed_ = true;
      if (!SetConsoleMode(output_, mode_ | ENABLE_PROCESSED_OUTPUT |
          ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        windows_error(GetLastError(), "enable virtual terminal output");
      }
      mode_changed_ = true;
    } catch (...) {
      restore();
      throw;
    }
  }

  ~TerminalDevice() { restore(); }

  void check_interrupt() const { check_wait(WaitForSingleObject(event_, 0)); }

  void wait_until(TimePoint until) const {
    do {
      check_wait(WaitForSingleObject(event_, timeout_ms(until)));
    } while (Clock::now() < until);
  }

  std::size_t write(std::span<const char> bytes, bool interruptible, TimePoint) const {
    if (interruptible) {
      set_writing_thread(thread_);
      try {
        check_interrupt();
      } catch (...) {
        set_writing_thread(nullptr);
        throw;
      }
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(std::min(bytes.size(), write_chunk));
    const auto ok = WriteFile(output_, bytes.data(), size, &written, nullptr);
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    if (interruptible) {
      set_writing_thread(nullptr);
      check_interrupt();
    }
    if (!ok) windows_error(error, "write terminal output");
    return written;
  }

private:
  static void check_wait(DWORD result) {
    if (result == WAIT_OBJECT_0) throw PlaybackInterrupted{};
    if (result == WAIT_FAILED) windows_error(GetLastError(), "wait for playback interrupt");
  }

  static void set_writing_thread(HANDLE thread) {
    AcquireSRWLockExclusive(&control_lock);
    writing_thread = thread;
    ReleaseSRWLockExclusive(&control_lock);
  }

  void restore() noexcept {
    if (mode_changed_) SetConsoleMode(output_, mode_);
    if (handler_installed_) SetConsoleCtrlHandler(wake_on_control, FALSE);
    AcquireSRWLockExclusive(&control_lock);
    writing_thread = nullptr;
    control_event = nullptr;
    ReleaseSRWLockExclusive(&control_lock);
    if (thread_) CloseHandle(thread_);
    if (event_) CloseHandle(event_);
  }

  HANDLE output_ = INVALID_HANDLE_VALUE;
  HANDLE event_ = nullptr;
  HANDLE thread_ = nullptr;
  DWORD mode_ = 0;
  bool handler_installed_ = false;
  bool mode_changed_ = false;
};

#else

[[noreturn]] void posix_error(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

// The local playback adapter owns the one SIGINT handler in this process.
// Track callbacks so a signal delivered to another thread cannot use a closed fd.
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> signal_fd{-1};
std::atomic<int> signal_callbacks{0};
std::atomic<int> interrupted{0};

void wake_on_signal(int) noexcept {
  const int error = errno;
  signal_callbacks.fetch_add(1);
  interrupted.store(1);
  const int fd = signal_fd.load();
  if (fd >= 0) {
    const char byte = 0;
    [[maybe_unused]] const auto ignored = ::write(fd, &byte, 1);
  }
  signal_callbacks.fetch_sub(1);
  errno = error;
}

class TerminalDevice {
public:
  TerminalDevice() {
    if (pipe(wake_.data()) == -1) posix_error("create playback interrupt pipe");
    try {
      for (const int fd : wake_) {
        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1 || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
          posix_error("configure playback interrupt pipe");
        }
      }
      previous_flags_ = fcntl(STDOUT_FILENO, F_GETFL);
      if (previous_flags_ == -1) posix_error("read terminal output flags");
      if (fcntl(STDOUT_FILENO, F_SETFL, previous_flags_ | O_NONBLOCK) == -1) {
        posix_error("configure terminal output");
      }
      flags_changed_ = true;
      struct sigaction action{};
      action.sa_handler = wake_on_signal;
      action.sa_flags = SA_RESTART;
      sigemptyset(&action.sa_mask);
      interrupted.store(0);
      signal_fd.store(wake_[1]);
      if (sigaction(SIGINT, &action, &previous_signal_) == -1) {
        posix_error("install playback interrupt handler");
      }
      handler_installed_ = true;
    } catch (...) {
      restore();
      throw;
    }
  }

  ~TerminalDevice() { restore(); }

  void check_interrupt() const {
    if (interrupted.load()) throw PlaybackInterrupted{};
  }

  void wait_until(TimePoint until) const {
    wait_ready(false, true, until);
  }

  std::size_t write(std::span<const char> bytes, bool interruptible, TimePoint until) const {
    const auto size = std::min(bytes.size(), write_chunk);
    while (true) {
      if (interruptible) check_interrupt();
      const auto written = ::write(STDOUT_FILENO, bytes.data(), size);
      if (written >= 0) return static_cast<std::size_t>(written);
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) posix_error("write terminal output");
      if ((!interruptible && Clock::now() >= until) || !wait_ready(true, interruptible, until)) {
        throw std::runtime_error("timed out restoring terminal output");
      }
    }
  }

private:
  bool wait_ready(bool output, bool interruptible, TimePoint until) const {
    pollfd fds[] = {{output ? STDOUT_FILENO : -1, POLLOUT, 0},
                    {interruptible ? wake_[0] : -1, POLLIN, 0}};
    while (true) {
      if (interruptible) check_interrupt();
      const auto timeout = until == TimePoint::max() ? -1 : timeout_ms(until);
      const auto ready = poll(fds, 2, timeout);
      if (interruptible) check_interrupt();
      if (ready < 0) {
        if (errno == EINTR) continue;
        posix_error("wait for terminal output");
      }
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("terminal output is unavailable");
      }
      if (fds[0].revents & POLLOUT) return true;
      if (Clock::now() >= until) return false;
    }
  }

  void restore() noexcept {
    signal_fd.store(-1);
    if (handler_installed_) sigaction(SIGINT, &previous_signal_, nullptr);
    while (signal_callbacks.load()) std::this_thread::yield();
    if (flags_changed_) fcntl(STDOUT_FILENO, F_SETFL, previous_flags_);
    for (const int fd : wake_) close(fd);
  }

  std::array<int, 2> wake_{};
  struct sigaction previous_signal_{};
  int previous_flags_ = 0;
  bool flags_changed_ = false;
  bool handler_installed_ = false;
};

#endif

bool same_size(Dimensions a, Dimensions b) {
  return a.width == b.width && a.height == b.height;
}

}

void hold_terminal_frame(TimePoint deadline, Dimensions bounds, const TerminalWait& wait,
                         const std::function<void(Dimensions)>& repaint) {
  constexpr auto interval = std::chrono::duration_cast<Clock::duration>(100ms);
  auto now = wait.now();
  while (now < deadline) {
    const auto viewport = wait.measure();
    if (viewport && !same_size(viewport->usable_pixels, bounds) && wait.now() < deadline) {
      bounds = viewport->usable_pixels;
      repaint(bounds);
    }
    now = wait.now();
    if (now >= deadline) break;
    wait.wait_until(now + std::min(deadline - now, interval));
    now = wait.now();
  }
}

struct TerminalPlayback::State {
  explicit State(Dimensions fallback) : cell_size(fallback) {}

  ~State() {
    try {
      restore_screen();
    } catch (...) {
      // Restore native modes on every exit, including an unavailable terminal.
    }
  }

  void control(std::string_view bytes, bool interruptible, TimePoint until = TimePoint::max()) {
    while (!bytes.empty()) {
      const auto count = device.write({bytes.data(), bytes.size()}, interruptible, until);
      if (!count) throw std::runtime_error("could not write terminal control sequence");
      bytes.remove_prefix(count);
    }
  }

  void restore_screen() {
    if (!entered) return;
    const auto until = Clock::now() + 1s;
    // If a write failed partway through a DCS, cancel it before sending CSI.
    if (writing_picture) control(cancel_picture, false, until);
    control(leave_screen, false, until);
    entered = false;
  }

  void draw(const DisplayFrame& frame, Dimensions new_bounds) {
    device.check_interrupt();
    const auto picture = fit_frame(frame, new_bounds, pixels);
    const Dimensions size{static_cast<int>(picture.width), static_cast<int>(picture.height)};
    if (raster && (!same_size(*raster, size) || !same_size(bounds, new_bounds))) {
      control(clear_screen, true);
    }
    control(home, true);
    writing_picture = true;
    encode_sixel(picture, {
      [&](std::span<const char> bytes) { return device.write(bytes, true, TimePoint::max()); },
      [] { return true; },
    });
    writing_picture = false;
    raster = size;
    bounds = new_bounds;
    device.check_interrupt();
  }

  TerminalDevice device;
  Dimensions cell_size;
  Dimensions bounds;
  std::optional<Dimensions> raster;
  std::vector<std::uint8_t> pixels;
  bool entered = false;
  bool writing_picture = false;
};

TerminalPlayback::TerminalPlayback(Dimensions cell_size)
    : state_(std::make_unique<State>(cell_size)) {
  state_->entered = true;
  state_->control(enter_screen, true);
}

TerminalPlayback::~TerminalPlayback() = default;

void TerminalPlayback::check_interrupt() const {
  state_->device.check_interrupt();
}

void TerminalPlayback::present(const DisplayFrame& frame) {
  check_interrupt();
  const auto viewport = measure_stdout(state_->cell_size);
  if (!viewport) throw std::runtime_error("terminal output is unavailable");
  state_->draw(frame, viewport->usable_pixels);
}

void TerminalPlayback::hold_until(TimePoint deadline, const DisplayFrame& frame) {
  check_interrupt();
  const TerminalWait wait{
    Clock::now,
    [&](auto until) { state_->device.wait_until(until); },
    [&] { return measure_stdout(state_->cell_size); },
  };
  hold_terminal_frame(deadline, state_->bounds, wait,
      [&](Dimensions bounds) { state_->draw(frame, bounds); });
}

void TerminalPlayback::finish() {
  state_->restore_screen();
  check_interrupt();
}

}
