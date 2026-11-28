#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace si {

struct DisplayOrientation {
  bool swap_axes = false;
  bool flip_x = false;
  bool flip_y = false;
};

struct DisplayFrame {
  int width = 0;
  int height = 0;
  // Pixels are packed RGB, already oriented and composited against black.
  std::vector<std::uint8_t> rgb;
  DisplayOrientation orientation;
  // Sample aspect in the oriented raster. Fitting applies it once.
  AVRational sample_aspect_ratio{1, 1};
  AVColorRange source_color_range = AVCOL_RANGE_UNSPECIFIED;
  AVColorSpace source_color_space = AVCOL_SPC_UNSPECIFIED;
  AVRational time_base{0, 1};
  std::optional<std::int64_t> pts;
  std::optional<std::int64_t> duration;
};

struct CaptureTime {
  std::chrono::sys_seconds instant;
  std::chrono::year_month_day date;
};

class MediaDecoder {
public:
  explicit MediaDecoder(const std::filesystem::path& path);
  ~MediaDecoder();

  // Returns owned frames in presentation order and nullopt at EOF.
  // Probe for motion by retaining the first frame and reading the second.
  std::optional<DisplayFrame> next_frame();
  std::optional<std::int64_t> declared_frame_count() const;
  AVRational frame_rate() const;
  // Call after decoding the first frame to include frame-level EXIF tags.
  std::optional<CaptureTime> capture_time() const;
  bool is_video() const;
  DisplayFrame video_preview(DisplayFrame first);

private:
  struct State;
  std::unique_ptr<State> state_;
};

}
