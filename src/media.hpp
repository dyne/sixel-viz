#pragma once

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

class MediaDecoder {
public:
  explicit MediaDecoder(const std::filesystem::path& path);
  ~MediaDecoder();

  // Returns owned frames in presentation order and nullopt at EOF.
  // Probe for motion by retaining the first frame and reading the second.
  std::optional<DisplayFrame> next_frame();
  std::optional<std::int64_t> declared_frame_count() const;
  AVRational frame_rate() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

}
