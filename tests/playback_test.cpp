#include "fit.hpp"
#include "playback.hpp"
#include "sixel.hpp"
#include "terminal_playback.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

namespace {
using namespace std::chrono_literals;
using Milliseconds = std::chrono::milliseconds;
using Sample = std::pair<int, Milliseconds>;
using Color = std::array<std::uint8_t, 3>;
using Identify = std::function<int(const si::DisplayFrame&)>;

constexpr Color red{255, 0, 0}, green{0, 255, 0}, blue{0, 0, 255};
constexpr Color white{255, 255, 255}, black{0, 0, 0}, yellow{255, 255, 0};
constexpr std::array gif_frames{
  "RRRRRRRR" "RRRRRRRR" "RRRRRRRR" "RRRRRRRR",
  "RRGRGRRR" "RRRGRGRR" "RRGRGRRR" "RRRRRRRR",
  "RRRRRRRR" "RRRRRRBB" "RRRRRBRB" "RRRRBBRR",
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> gif_pixels(std::string_view pixels) {
  std::vector<std::uint8_t> result;
  for (const char code : pixels) {
    const auto& color = code == 'R' ? red : code == 'G' ? green : blue;
    result.insert(result.end(), color.begin(), color.end());
  }
  return result;
}

int gif_id(const si::DisplayFrame& frame) {
  require(frame.width == 8 && frame.height == 4, "GIF canvas dimensions");
  for (int index = 0; index < static_cast<int>(gif_frames.size()); ++index) {
    if (frame.rgb == gif_pixels(gif_frames[index])) return index + 1;
  }
  throw std::runtime_error("GIF pixels differ from the composed reference");
}

int solid_color_id(const si::DisplayFrame& frame) {
  require(frame.width == 8 && frame.height == 4 && frame.rgb.size() == 96,
          "variable-rate raster dimensions");
  constexpr std::array colors{red, green, blue, white};
  const auto color = std::ranges::find_if(colors, [&](const Color& sample) {
    for (std::size_t offset = 0; offset < frame.rgb.size(); offset += sample.size()) {
      if (!std::ranges::equal(sample, std::span{frame.rgb}.subspan(offset, sample.size()))) {
        return false;
      }
    }
    return true;
  });
  require(color != colors.end(), "unexpected solid color");
  return static_cast<int>(color - colors.begin()) + 1;
}

int h264_id(const si::DisplayFrame& frame) {
  require(frame.width == 32 && frame.height == 16 && frame.rgb.size() == 1536,
          "H.264 raster dimensions");
  const auto pixel = std::span{frame.rgb}.subspan((4 * 32 + 24) * 3, 3);
  constexpr std::array colors{red, green, blue, white, black, yellow};
  const auto color = std::ranges::find_if(colors, [&](const Color& sample) {
    return std::ranges::equal(sample, pixel,
        [](int a, int b) { return std::abs(a - b) <= 6; });
  });
  require(color != colors.end(), "unexpected H.264 marker color");
  return static_cast<int>(color - colors.begin()) + 1;
}

struct Trace {
  std::vector<int> decoded;
  std::vector<Sample> shown;
  std::vector<Sample> holds;
  std::vector<std::size_t> decoded_at_show;
  Milliseconds elapsed{};
  int clock_reads = 0;
  int eof_count = 0;
};

Trace play(const si::NextFrame& source, AVRational rate, const Identify& identify,
           bool terminal = true, Milliseconds first_write = 0ms) {
  Trace trace;
  const si::PlaybackClock::TimePoint origin{3s};
  auto now = origin;
  const auto time = [&](auto point) { return std::chrono::duration_cast<Milliseconds>(point - origin); };
  const auto next = [&] {
    auto frame = source();
    if (frame) trace.decoded.push_back(identify(*frame));
    else ++trace.eof_count;
    return frame;
  };
  const auto present = [&](const si::DisplayFrame& frame) {
    trace.shown.emplace_back(identify(frame), time(now));
    trace.decoded_at_show.push_back(trace.decoded.size());
    if (trace.shown.size() == 1) now += first_write;
  };
  const si::PlaybackClock clock{
    [&] { ++trace.clock_reads; return now; },
    [&](auto until, const si::DisplayFrame& held) {
      const int id = identify(held);
      require(until > now && !trace.shown.empty() && trace.shown.back().first == id,
              "wait must retain the current displayed frame and a future deadline");
      trace.holds.emplace_back(id, time(until));
      now = until;
    },
  };
  auto first = next();
  auto second = next();
  require(first.has_value() && second.has_value(), "motion probe must retain two frames");
  si::play_once(std::move(*first), std::move(*second), rate, next, present, terminal ? &clock : nullptr);
  trace.elapsed = time(now);
  require(trace.eof_count == 1, "expected exactly one EOF read");
  return trace;
}

void expect_fast(const Trace& trace, std::initializer_list<Sample> shown, Milliseconds ends) {
  require(trace.shown == std::vector<Sample>{shown}, "unexpected frame presentation order or deadline");
  require(trace.elapsed == ends && trace.holds.size() == shown.size(), "unexpected final frame interval");
  require(trace.decoded.size() == shown.size(), "unexpected decoded frame count");
  for (std::size_t step = 0; step < trace.shown.size(); ++step) {
    const auto [id, at] = trace.shown[step];
    const auto until = step + 1 == trace.shown.size() ? ends : trace.shown[step + 1].second;
    require(trace.decoded[step] == id && trace.holds[step] == Sample{id, until},
            "unexpected decoder order or held frame");
    const auto decoded = step == 0 ? 2 : std::min(step + 2, trace.shown.size());
    require(trace.decoded_at_show[step] == decoded, "presentation exceeded one frame of lookahead");
  }
}

Trace play_file(const char* path, const Identify& identify, bool terminal = true,
                Milliseconds first_write = 0ms, bool missing_pts = false) {
  si::MediaDecoder decoder{path};
  return play([&] {
    auto frame = decoder.next_frame();
    if (frame && missing_pts) require(!frame->pts, "expected an untimestamped H.264 stream");
    return frame;
  }, decoder.frame_rate(), identify, terminal, first_write);
}

void invalid_timestamps() {
  const auto synthetic = [](AVRational rate) {
    int n = 0;
    return play([&]() -> std::optional<si::DisplayFrame> {
      if (n == 7) return std::nullopt;
      constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
      constexpr std::array<std::optional<std::int64_t>, 7> ticks{
        std::nullopt, -20, -20, -21, maximum, 4000, std::nullopt,
      };
      si::DisplayFrame frame;
      frame.time_base = {1, 1000};
      frame.pts = ticks[n];
      frame.rgb.push_back(++n);
      return frame;
    }, rate, [](const auto& frame) { return frame.rgb.front(); });
  };
  expect_fast(synthetic({0, 0}),
      {{1, 0ms}, {2, 40ms}, {3, 80ms}, {4, 120ms}, {5, 160ms}, {6, 4060ms}, {7, 4100ms}},
      4140ms);
  expect_fast(synthetic({1, 2000}),
      {{1, 0ms}, {2, 1000ms}, {3, 2000ms}, {4, 3000ms}, {5, 4000ms}, {6, 5020ms}, {7, 6020ms}},
      7020ms);
  expect_fast(synthetic({2'000'000'000, 1}),
      {{1, 0ms}, {2, 1ms}, {3, 2ms}, {4, 3ms}, {5, 4ms}, {6, 4021ms}, {7, 4022ms}},
      4023ms);
}

void check_redirected_bytes(const char* path) {
  std::ifstream stream{path, std::ios::binary};
  require(stream.good(), "cannot read CLI SIXEL capture");
  const std::string output{std::istreambuf_iterator<char>{stream}, {}};
  std::size_t offset = 0;
  for (const auto* reference : gif_frames) {
    std::string expected;
    const auto pixels = gif_pixels(reference);
    si::encode_sixel({pixels, 8, 4, 24}, {
      [&](std::span<const char> bytes) {
        expected.append(bytes.data(), bytes.size());
        return bytes.size();
      },
      [] { return true; },
    });
    const std::string_view picture{expected};
    require(picture.starts_with("\033P") && picture.ends_with("\033\\"),
            "reference contains an incomplete SIXEL picture");
    require(std::string_view{output}.substr(offset, picture.size()) == picture,
            "redirected frame differs from the expected SIXEL picture");
    offset += picture.size();
  }
  const auto caption = std::string_view{output}.substr(offset);
  require(caption.starts_with('\n') && caption.ends_with(" once-disposal.gif\n\n") &&
          std::count(caption.begin(), caption.end(), '\n') == 3 &&
          caption.find('\033') == caption.npos,
          "redirected GIF must end with one caption and a blank line");
}

void check_resize(const char* path) {
  si::MediaDecoder decoder{path};
  auto first = decoder.next_frame();
  auto second = decoder.next_frame();
  require(first && second, "resize fixture must contain motion");

  const si::PlaybackClock::TimePoint origin{3s};
  auto now = origin;
  const auto elapsed = [&] { return std::chrono::duration_cast<Milliseconds>(now - origin); };
  const si::TerminalWait wait{
    [&] { return now; },
    [&](auto until) {
      require(until - now <= 100ms, "resize poll exceeded 100 ms");
      now = until;
    },
    [&]() -> std::optional<si::Viewport> {
      if (elapsed() < 300ms) return si::Viewport{{8, 4}, false};
      if (elapsed() < 575ms) return si::Viewport{{4, 4}, false};
      return si::Viewport{{7, 4}, false};
    },
  };
  using Render = std::tuple<int, Milliseconds, std::size_t, std::size_t>;
  std::vector<Render> drawn;
  std::vector<std::uint8_t> scaled;
  si::Dimensions shown;
  const auto draw = [&](const si::DisplayFrame& frame, si::Dimensions bounds) {
    const auto fitted = si::fit_frame(frame, bounds, scaled);
    drawn.emplace_back(gif_id(frame), elapsed(), fitted.width, fitted.height);
    shown = bounds;
  };
  const si::PlaybackClock clock{
    wait.now,
    [&](auto deadline, const si::DisplayFrame& held) {
      si::hold_terminal_frame(deadline, shown, wait, [&](si::Dimensions bounds) {
        draw(held, bounds);
        now += 15ms;
      });
    },
  };
  int decoded = 2;
  int eof = 0;
  si::play_once(std::move(*first), std::move(*second), decoder.frame_rate(), [&] {
    auto frame = decoder.next_frame();
    if (frame) ++decoded;
    else ++eof;
    return frame;
  }, [&](const si::DisplayFrame& frame) { draw(frame, wait.measure()->usable_pixels); }, &clock);
  const std::vector<Render> expected{
    {1, 0ms, 8, 4}, {1, 300ms, 4, 2}, {1, 615ms, 7, 3},
    {2, 2000ms, 7, 3}, {3, 2120ms, 7, 3},
  };
  require(drawn == expected, "resize must repaint the held frame and preserve its deadline");
  require(elapsed() == 2430ms && decoded == 3 && eof == 1,
          "repaint changed GIF frame count, EOF or final duration");
}

void check_cancel_before_pending() {
  for (const bool enters_write : {true, false}) {
    bool registered = true;
    bool pending = false;
    bool cancelled = false;
    int attempts = 0;
    int pauses = 0;
    si::detail::retry_console_cancellation([&] {
      ++attempts;
      if (!registered) return false;
      if (!pending) return true;
      cancelled = true;
      return false;
    }, [&] {
      require(++pauses == 1 && attempts == 1, "expected one retry after no pending write");
      // The writer either enters the kernel or withdraws before its cleanup.
      pending = enters_write;
      registered = enters_write;
    });
    require(attempts == 2 && cancelled == enters_write,
            "console cancellation lost a pending write or reached a withdrawn writer");
  }
}

}

int main(int argc, char** argv) {
  av_log_set_level(AV_LOG_QUIET);
  try {
    require(argc >= 2, "playback case is required");
    const std::string_view test = argv[1];
    if (test == "invalid_timestamps") {
      invalid_timestamps();
      return 0;
    }
    if (test == "cancel_before_pending") {
      check_cancel_before_pending();
      return 0;
    }
    require(argc == 3, "fixture or capture path is required");
    if (test == "variable_rate") {
      expect_fast(play_file(argv[2], solid_color_id),
          {{1, 0ms}, {2, 120ms}, {3, 360ms}, {4, 400ms}}, 440ms);
    } else if (test == "reordered" || test == "missing_timestamps") {
      expect_fast(play_file(argv[2], h264_id, true, 0ms, test == "missing_timestamps"),
          {{1, 0ms}, {2, 125ms}, {3, 250ms}, {4, 375ms}, {5, 500ms}, {6, 625ms}}, 750ms);
    } else if (test == "gif") {
      expect_fast(play_file(argv[2], gif_id), {{1, 0ms}, {2, 2000ms}, {3, 2120ms}}, 2430ms);
    } else if (test == "slow") {
      const auto trace = play_file(argv[2], h264_id, true, 320ms);
      require(trace.decoded == std::vector{1, 2, 3, 4, 5, 6}, "slow sink lost a decoded frame");
      require(trace.shown == std::vector<Sample>{{1, 0ms}, {3, 320ms}, {4, 375ms},
          {5, 500ms}, {6, 625ms}}, "slow sink did not skip expired frames on the original timeline");
      require(trace.elapsed == 750ms, "slow sink shifted the end of the timeline");
      const auto stalled = play_file(argv[2], h264_id, true, 700ms);
      require(stalled.decoded == trace.decoded &&
              stalled.shown == std::vector<Sample>{{1, 0ms}, {6, 700ms}} &&
              stalled.holds == std::vector<Sample>{{6, 825ms}}, "late final picture lost its hold");
    } else if (test == "redirected") {
      const auto trace = play_file(argv[2], h264_id, false, 800ms);
      require(trace.decoded == std::vector{1, 2, 3, 4, 5, 6} &&
              trace.shown == std::vector<Sample>{{1, 0ms}, {2, 800ms}, {3, 800ms},
                  {4, 800ms}, {5, 800ms}, {6, 800ms}}, "redirected output lost an ordered frame");
      require(trace.clock_reads == 0 && trace.holds.empty() && trace.elapsed == 800ms,
              "redirected output used interactive pacing");
    } else if (test == "redirected_bytes") {
      check_redirected_bytes(argv[2]);
    } else if (test == "resize") {
      check_resize(argv[2]);
    } else {
      throw std::runtime_error("unknown playback case");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
