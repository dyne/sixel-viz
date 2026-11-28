#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto shell_before = "SHELL-BEFORE";
constexpr auto shell_after = "SHELL-AFTER=";
constexpr auto start_screen = "\033[?1049h\033[?25l";
constexpr auto end_screen = "\033[?1049l\033[?25h";
constexpr auto home = "\033[1;1H";
constexpr auto erase = "\033[2J";
constexpr auto dcs = "\033P";
constexpr auto st = "\033\\";
constexpr auto cancel = "\030\033\\";

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

class PtyProcess {
public:
  PtyProcess(const char* program, const char* input) {
    winsize initial{30, 80, 640, 480};
    child_ = forkpty(&master_, nullptr, nullptr, &initial);
    require(child_ >= 0, "could not start a process on a pseudoterminal");
    if (child_ == 0) {
      termios settings{};
      if (tcgetattr(STDIN_FILENO, &settings) < 0) _exit(90);
      settings.c_lflag = (settings.c_lflag | ISIG | NOFLSH) & ~ECHO;
      settings.c_cc[VINTR] = 3;
      if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) < 0) _exit(91);
      execl("/bin/sh", "sh", "-c",
            "trap ':' INT; printf 'SHELL-BEFORE\\n'; \"$1\" \"$2\"; "
            "status=$?; printf 'SHELL-AFTER=%s\\n' \"$status\"; exit \"$status\"",
            "sh", program, input, static_cast<char*>(nullptr));
      _exit(92);
    }
  }

  ~PtyProcess() {
    if (child_ > 0) {
      kill(-child_, SIGKILL);
      kill(child_, SIGKILL);
      while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    close(master_);
  }

  std::size_t read_through(std::string_view marker, std::size_t start,
                           std::chrono::milliseconds budget) {
    const auto deadline = Clock::now() + budget;
    while (true) {
      if (const auto found = captured.find(marker, start); found != std::string::npos) return found;
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      require(remaining.count() > 0, "timed out waiting for terminal bytes");
      pollfd input{master_, POLLIN, 0};
      const int result = poll(&input, 1, static_cast<int>(remaining.count()));
      if (result < 0 && errno == EINTR) continue;
      require(result > 0 && (input.revents & POLLIN), "terminal closed or stopped producing output");
      char data[4096];
      const auto count = read(master_, data, sizeof(data));
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "could not read captured terminal bytes");
      captured.append(data, static_cast<std::size_t>(count));
    }
  }

  void resize(winsize size) {
    require(ioctl(master_, TIOCSWINSZ, &size) == 0, "could not resize pseudoterminal");
  }

  void interrupt() {
    require(write(master_, "\003", 1) == 1, "could not type Ctrl+C");
  }

  void expect_return(int code, std::chrono::milliseconds budget, bool playback = true) {
    const auto prompt = read_through(std::string{shell_after} + std::to_string(code), 0, budget);
    const auto entered = captured.find(start_screen);
    const auto left = captured.find(end_screen);
    if (playback) {
      require(captured.find(shell_before) < entered && entered < left && left < prompt,
              "shell text must surround one alternate-screen session");
      require(captured.find(start_screen, entered + 1) == std::string::npos &&
              captured.find(end_screen, left + 1) == std::string::npos,
              "alternate screen entered or left more than once");
    } else {
      require(captured.find(shell_before) < prompt && entered == std::string::npos &&
              left == std::string::npos, "gallery needs to stay on the primary screen");
    }
    require(captured.find("si: ") == std::string::npos, "CLI emitted an error diagnostic");
    int status = 0;
    pid_t exited;
    do {
      exited = waitpid(child_, &status, 0);
    } while (exited < 0 && errno == EINTR);
    require(exited == child_, "could not read shell exit status");
    child_ = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == code, "unexpected shell exit status");
  }

  std::string captured;

private:
  int master_ = -1;
  pid_t child_ = -1;
};

int parameter(std::string_view& bytes) {
  int value = 0;
  const auto [end, err] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
  require(err == std::errc{}, "malformed SIXEL raster parameters");
  bytes.remove_prefix(end - bytes.data());
  return value;
}

std::size_t expect_picture(PtyProcess& process, std::size_t offset, int width, int height,
                           bool cleared, std::chrono::milliseconds budget) {
  const auto start = process.read_through(dcs, offset, budget);
  const auto end = process.read_through(st, start + 2, budget);
  const auto prefix = std::string_view{process.captured}.substr(offset, start - offset);
  require(prefix.ends_with(home), "SIXEL picture did not start at terminal origin");
  require((prefix.find(erase) != std::string_view::npos) == cleared,
          "stale raster clearing did not match the size change");
  auto picture = std::string_view{process.captured}.substr(start, end - start);
  const auto raster = picture.find('"');
  require(raster != std::string_view::npos, "missing SIXEL raster attributes");
  picture.remove_prefix(raster + 1);
  for (int part = 0; part < 2; ++part) {
    require(parameter(picture) > 0 && picture.starts_with(';'), "invalid SIXEL pixel aspect");
    picture.remove_prefix(1);
  }
  require(parameter(picture) == width && picture.starts_with(';'), "unexpected SIXEL width");
  picture.remove_prefix(1);
  require(parameter(picture) == height, "unexpected SIXEL height");
  return end + 2;
}

void check_resize(PtyProcess& process) {
  auto next = expect_picture(process, 0, 480, 300, false, 2500ms);
  process.resize({20, 34, 272, 320});
  next = expect_picture(process, next, 272, 170, true, 900ms);
  process.resize({25, 50, 400, 400});
  next = expect_picture(process, next, 400, 250, true, 900ms);
  next = expect_picture(process, next, 400, 250, false, 5000ms);
  next = expect_picture(process, next, 400, 250, false, 1500ms);
  process.expect_return(0, 1500ms);
  const auto left = process.captured.find(end_screen);
  const auto still = process.captured.find(dcs, next);
  require(left < still && still < process.captured.find(shell_after), "expected a still after playback");
  const auto end = process.captured.find(st, still);
  require(end != std::string::npos, "expected a complete still after playback");
  const auto caption = std::string_view{process.captured}.substr(end + 2);
  require(caption.starts_with("\0338") && caption.size() > 10 &&
          caption[4] == '/' && caption[7] == '/' &&
          process.captured.find(dcs, still + 2) == std::string::npos,
          "expected one final still and caption on the primary screen");
  std::cout << "PTY frames 480x300, 272x170, 400x250, 400x250, 400x250; shell returned 0\n";
}

void check_gallery(PtyProcess& process) {
  process.expect_return(0, 2500ms, false);
  const auto start = process.captured.find(dcs);
  const auto end = process.captured.find(st, start);
  require(start < end && end != std::string::npos, "missing first SIXEL preview");
  const auto right = process.captured.find(dcs, end + 2);
  const auto right_end = process.captured.find(st, right);
  require(right < right_end && right_end != std::string::npos, "missing second SIXEL preview");
  const auto third = process.captured.find(dcs, right_end + 2);
  const auto third_end = process.captured.find(st, third);
  require(third < third_end && third_end != std::string::npos, "missing third SIXEL preview");
  const auto bytes = std::string_view{process.captured};
  const auto after_left = bytes.substr(end + 2, right - end - 2);
  require(after_left.starts_with("\0338\033[2A") &&
          after_left.find("catalog/left.png") != std::string_view::npos &&
          after_left.ends_with("\033[29A\033[41G"),
          "expected the left caption below its preview and the tall preview on the right");
  const auto after_right = bytes.substr(right_end + 2, third - right_end - 2);
  // The portrait spans 27 raster rows, the PNG one. Their tile ends are
  // 26 rows apart, so the third preview can start immediately on the left.
  require(after_right.starts_with("\0338\033[2A\033[41G") &&
          after_right.find("catalog/right.svg") != std::string_view::npos &&
          after_right.ends_with("\0338\r\0337\033[26A"),
          "expected the next preview in the shorter column without scrolling");
  const auto after_third = bytes.substr(third_end + 2);
  require(after_third.starts_with("\0338\033[25A") &&
          after_third.find("catalog/third.gif") < after_third.find(shell_after),
          "expected the third caption below its preview and the prompt below the gallery");
  std::cout << "PTY gallery tiles flow independently with captions below each preview\n";
}

void check_hold_interrupt(PtyProcess& process) {
  const auto first = expect_picture(process, 0, 8, 4, false, 2500ms);
  process.interrupt();
  process.expect_return(130, 1000ms);
  require(process.captured.find(dcs, first) == std::string::npos &&
          process.captured.find(cancel, first) == std::string::npos,
          "terminal hold interruption altered a completed SIXEL picture");
  std::cout << "Ctrl+C woke a frame hold and restored the shell with status 130\n";
}

void check_write_interrupt(PtyProcess& process) {
  const auto first = process.read_through(dcs, 0, 2500ms);
  process.read_through("\"", first + 2, 400ms);
  // Leave the master unread so this striped raster fills the PTY queue.
  std::this_thread::sleep_for(120ms);
  process.interrupt();
  process.expect_return(130, 1500ms);
  const auto cancelled = process.captured.find(cancel, first);
  require(cancelled != std::string::npos && cancelled < process.captured.find(end_screen),
          "interruption did not close the in-progress SIXEL before restoring the terminal");
  require(process.captured.find(st, first + 2) == cancelled + 1,
          "the SIXEL picture completed before interrupt was sent");
  require(process.captured.find(dcs, first + 2) == std::string::npos,
          "output continued with another picture after interrupt");
  std::cout << "Ctrl+C cancelled a partial SIXEL after " << cancelled - first
            << " bytes and restored the shell with status 130\n";
}

}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    PtyProcess process{argv[2], argv[3]};
    const std::string_view test = argv[1];
    if (test == "resize") check_resize(process);
    else if (test == "gallery") check_gallery(process);
    else if (test == "interrupt_hold") check_hold_interrupt(process);
    else if (test == "interrupt_write") check_write_interrupt(process);
    else return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
