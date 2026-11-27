#include "fit.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
}

namespace si {
namespace {

constexpr std::size_t channels = 3;

int pixel_extent(std::int64_t extent) {
  if (extent > std::numeric_limits<int>::max()) {
    throw std::runtime_error("display dimensions are out of range");
  }
  return static_cast<int>(std::max<std::int64_t>(1, extent));
}

std::size_t sample_center(std::size_t output, int source_size, int target_size) {
  return static_cast<std::size_t>(
      (2 * static_cast<std::int64_t>(output) + 1) * source_size /
      (2 * static_cast<std::int64_t>(target_size)));
}

}

Dimensions fit_dimensions(Dimensions source, AVRational sample_aspect,
    std::optional<Dimensions> viewport) {
  if (source.width < 1 || source.height < 1 ||
      (viewport && (viewport->width < 1 || viewport->height < 1))) {
    throw std::runtime_error("image or viewport dimensions are out of range");
  }
  if (sample_aspect.num < 1 || sample_aspect.den < 1) sample_aspect = {1, 1};

  const auto native_width = std::max<std::int64_t>(1,
      av_rescale_rnd(source.width, sample_aspect.num, sample_aspect.den, AV_ROUND_NEAR_INF));
  if (!viewport || (native_width <= viewport->width && source.height <= viewport->height)) {
    return {pixel_extent(native_width), source.height};
  }

  const auto width_units = static_cast<std::int64_t>(source.width) * sample_aspect.num;
  const auto height_units = static_cast<std::int64_t>(source.height) * sample_aspect.den;
  const int height = std::min(source.height, viewport->height);
  // Compare the limiting axes using the exact display ratio, then round down.
  if (av_rescale_rnd(height, width_units, height_units, AV_ROUND_UP) <= viewport->width) {
    return {pixel_extent(av_rescale_rnd(height, width_units, height_units, AV_ROUND_DOWN)), height};
  }
  return {viewport->width,
          pixel_extent(av_rescale_rnd(viewport->width, height_units, width_units, AV_ROUND_DOWN))};
}

RgbView fit_frame(const DisplayFrame& frame, std::optional<Dimensions> viewport,
    std::vector<std::uint8_t>& scaled_rgb) {
  const auto target = fit_dimensions({frame.width, frame.height}, frame.sample_aspect_ratio, viewport);
  const auto source_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame.width, frame.height, 1);
  if (source_size < 0 || static_cast<std::size_t>(source_size) > frame.rgb.size()) {
    throw std::runtime_error("invalid decoded RGB buffer size");
  }

  const auto width = static_cast<std::size_t>(target.width);
  const auto height = static_cast<std::size_t>(target.height);
  const auto byte_count = sixel_rgb_size(width, height);
  const auto stride = width * channels;
  if (target.width == frame.width && target.height == frame.height) {
    return {frame.rgb, width, height, stride};
  }

  scaled_rgb.resize(byte_count);
  const auto source_stride = static_cast<std::size_t>(frame.width) * channels;
  for (std::size_t y = 0; y < height; ++y) {
    const auto* source = frame.rgb.data() + sample_center(y, frame.height, target.height) * source_stride;
    auto* destination = scaled_rgb.data() + y * stride;
    for (std::size_t x = 0; x < width; ++x) {
      const auto source_x = sample_center(x, frame.width, target.width);
      std::copy_n(source + source_x * channels, channels, destination + x * channels);
    }
  }
  return {scaled_rgb, width, height, stride};
}

}
