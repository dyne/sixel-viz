#pragma once

#include "media.hpp"

#include <chrono>
#include <functional>

namespace si {

struct PlaybackClock {
  using TimePoint = std::chrono::steady_clock::time_point;

  std::function<TimePoint()> now;
  // The frame remains on screen throughout this wait and can be repainted.
  std::function<void(TimePoint, const DisplayFrame&)> wait_until;
};

using NextFrame = std::function<std::optional<DisplayFrame>()>;
using PresentFrame = std::function<void(const DisplayFrame&)>;

// Passing no clock streams every frame to a redirected sink. A terminal
// keeps the displayed frame, one pending presentation and one lookahead.
void play_once(DisplayFrame first, DisplayFrame second, AVRational frame_rate,
               const NextFrame& next_frame, const PresentFrame& present,
               const PlaybackClock* clock);

}
