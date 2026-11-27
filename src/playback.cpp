#include "playback.hpp"

#include <algorithm>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace si {
namespace {

using TimePoint = PlaybackClock::TimePoint;
using Microseconds = std::chrono::microseconds;
using namespace std::chrono_literals;

constexpr AVRational microsecond_base{1, 1'000'000};
// Leave headroom for converting relative offsets back to the native clock.
constexpr auto maximum_offset = std::chrono::duration_cast<Microseconds>(
    TimePoint::duration::max() / 2);

bool positive(AVRational ratio) {
  return ratio.num > 0 && ratio.den > 0;
}

std::optional<Microseconds> media_time(std::int64_t ticks, AVRational time_base) {
  if (ticks <= 0 || !positive(time_base)) return std::nullopt;
  const Microseconds scaled{av_rescale_q(ticks, time_base, microsecond_base)};
  if (scaled <= 0us || scaled > maximum_offset) return std::nullopt;
  return scaled;
}

Microseconds fallback_interval(AVRational rate) {
  if (!positive(rate)) return 40ms;
  const Microseconds interval{av_rescale_q(1, {rate.den, rate.num}, microsecond_base)};
  return std::clamp(interval, Microseconds{1ms}, Microseconds{1s});
}

TimePoint deadline(TimePoint origin, Microseconds offset) {
  const auto delta = std::chrono::duration_cast<TimePoint::duration>(offset);
  return origin > TimePoint::max() - delta ? TimePoint::max() : origin + delta;
}

class Timeline {
public:
  Timeline(const DisplayFrame& first, AVRational rate) : fallback_(fallback_interval(rate)) {
    set_anchor(first);
  }

  Microseconds advance(const DisplayFrame& frame) {
    const auto timestamp = offset_from_anchor(frame);
    if (timestamp && *timestamp > offset_) {
      offset_ = *timestamp;
    } else {
      offset_ += std::min(fallback_, maximum_offset - offset_);
    }
    if (!anchor_pts_) set_anchor(frame);
    return offset_;
  }

  Microseconds final_duration(const DisplayFrame& frame) const {
    return media_time(frame.duration.value_or(0), frame.time_base).value_or(fallback_);
  }

private:
  void set_anchor(const DisplayFrame& frame) {
    if (frame.pts && positive(frame.time_base)) {
      anchor_pts_ = frame.pts;
      anchor_base_ = frame.time_base;
      anchor_offset_ = offset_;
    }
  }

  std::optional<Microseconds> offset_from_anchor(const DisplayFrame& frame) const {
    if (!anchor_pts_ || !frame.pts || *frame.pts <= *anchor_pts_ ||
        frame.time_base.num != anchor_base_.num || frame.time_base.den != anchor_base_.den) {
      return std::nullopt;
    }
    // Unsigned subtraction also handles anchors and timestamps of opposite signs.
    const auto ticks = static_cast<std::uint64_t>(*frame.pts) -
                       static_cast<std::uint64_t>(*anchor_pts_);
    if (ticks > std::numeric_limits<std::int64_t>::max()) return std::nullopt;
    const auto elapsed = media_time(static_cast<std::int64_t>(ticks), frame.time_base);
    if (!elapsed || *elapsed > maximum_offset - anchor_offset_) return std::nullopt;
    return anchor_offset_ + *elapsed;
  }

  Microseconds fallback_;
  Microseconds offset_{};
  std::optional<std::int64_t> anchor_pts_;
  AVRational anchor_base_{0, 1};
  Microseconds anchor_offset_{};
};

void hold_until(const PlaybackClock& clock, TimePoint until, const DisplayFrame& held) {
  if (clock.now() < until) clock.wait_until(until, held);
}

}

void play_once(DisplayFrame held, DisplayFrame pending, AVRational frame_rate,
               const NextFrame& next_frame, const PresentFrame& present,
               const PlaybackClock* clock) {
  if (!clock) {
    present(held);
    present(pending);
    while (const auto next = next_frame()) present(*next);
    return;
  }

  Timeline timeline{held, frame_rate};
  auto offset = timeline.advance(pending);
  const auto origin = clock->now();
  present(held);
  while (true) {
    auto next = next_frame();
    const auto starts = deadline(origin, offset);
    if (!next) {
      hold_until(*clock, starts, held);
      present(pending);
      // With no successor, hold the fully written picture for its display interval.
      const auto ends = deadline(clock->now(), timeline.final_duration(pending));
      hold_until(*clock, ends, pending);
      return;
    }

    offset = timeline.advance(*next);
    hold_until(*clock, starts, held);
    if (clock->now() < deadline(origin, offset)) {
      present(pending);
      held = std::move(pending);
    }
    pending = std::move(*next);
  }
}

}
