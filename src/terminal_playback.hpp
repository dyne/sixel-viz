#pragma once

#include "terminal.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace si {

struct DisplayFrame;

struct PlaybackInterrupted {};

namespace detail {

// Retry a registered console write while synchronous I/O is not yet pending.
template<class TryCancel, class Pause>
void retry_console_cancellation(TryCancel try_cancel, Pause pause) {
  while (try_cancel()) pause();
}

}

struct TerminalWait {
  using TimePoint = std::chrono::steady_clock::time_point;

  std::function<TimePoint()> now;
  std::function<void(TimePoint)> wait_until;
  std::function<std::optional<Viewport>()> measure;
};

void hold_terminal_frame(TerminalWait::TimePoint deadline, Dimensions bounds,
                         const TerminalWait& wait,
                         const std::function<void(Dimensions)>& repaint);

class TerminalPlayback {
public:
  explicit TerminalPlayback(Dimensions cell_size);
  ~TerminalPlayback();

  void check_interrupt() const;
  void present(const DisplayFrame& frame);
  void hold_until(TerminalWait::TimePoint deadline, const DisplayFrame& frame);
  void finish();

private:
  struct State;
  std::unique_ptr<State> state_;
};

}
