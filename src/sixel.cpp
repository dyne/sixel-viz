#include "sixel.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <sixel.h>

namespace si {
namespace {
constexpr int channels = 3;
constexpr int palette_size = 256;

void check_sixel(SIXELSTATUS status, const char* operation) {
  if (SIXEL_FAILED(status)) {
    throw std::runtime_error(std::string{operation} + ": " + sixel_helper_format_error(status));
  }
}

std::size_t checked_row_size(const RgbView& image) {
  const auto byte_count = sixel_rgb_size(image.width, image.height);
  const auto row_size = byte_count / image.height;
  // Only the visible part of the last row needs to be present.
  if (image.stride < row_size || image.bytes.size() < row_size ||
      image.height - 1 > (image.bytes.size() - row_size) / image.stride) {
    throw std::runtime_error("SIXEL image buffer is too small for its dimensions and stride");
  }
  return row_size;
}

struct Output {
  const SixelSink& sink;
  bool first_band_is_partial;
  std::exception_ptr failure;

  void write_all(std::span<const char> remaining) {
    while (!remaining.empty()) {
      const auto written = sink.write(remaining);
      if (written == 0 || written > remaining.size()) {
        throw std::runtime_error("could not write SIXEL picture");
      }
      remaining = remaining.subspan(written);
    }
  }

  static int write(char* data, int size, void* opaque) noexcept {
    auto& state = *static_cast<Output*>(opaque);
    if (state.failure) return -1;
    try {
      auto remaining = std::span<const char>{data, static_cast<std::size_t>(size)};
      if (state.first_band_is_partial) {
        // libsixel 1.8.7 emits a graphics next-line command before the only
        // band of a picture shorter than six pixels. Its header, palette
        // and sixel data contain no other '-' bytes for a one-band raster.
        const auto newline = std::find(remaining.begin(), remaining.end(), '-');
        if (newline != remaining.end()) {
          const auto offset = static_cast<std::size_t>(newline - remaining.begin());
          state.first_band_is_partial = false;
          state.write_all(remaining.first(offset));
          remaining = remaining.subspan(offset + 1);
        }
      }
      state.write_all(remaining);
      return size;
    } catch (...) {
      // Some libsixel callers ignore the callback result. Save the first
      // failure and stop handing bytes to the sink for this picture.
      state.failure = std::current_exception();
      return -1;
    }
  }
};
}

std::size_t sixel_rgb_size(std::size_t width, std::size_t height) {
  if (!width || !height || width > SIXEL_WIDTH_LIMIT || height > SIXEL_HEIGHT_LIMIT) {
    throw std::runtime_error("SIXEL image dimensions are out of range");
  }
  // libsixel performs packed RGB byte-count arithmetic in signed int.
  constexpr auto max_size = static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (width > max_size / channels / height) {
    throw std::runtime_error("SIXEL image byte count is out of range");
  }
  return width * height * channels;
}

void encode_sixel(RgbView image, const SixelSink& sink) {
  const auto row_size = checked_row_size(image);
  std::vector<std::uint8_t> packed;
  const auto* pixels = image.bytes.data();
  if (image.stride != row_size) {
    packed.resize(row_size * image.height);
    for (std::size_t y = 0; y < image.height; ++y) {
      std::copy_n(pixels + y * image.stride, row_size, packed.data() + y * row_size);
    }
    pixels = packed.data();
  }

  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  sixel_dither_t* raw_dither = nullptr;
  check_sixel(sixel_dither_new(&raw_dither, palette_size, nullptr), "create SIXEL dither");
  const std::unique_ptr<sixel_dither_t, decltype(&sixel_dither_unref)>
      dither{raw_dither, sixel_dither_unref};
  // The C API accepts a mutable pointer. With RGB888 and diffusion disabled
  // the palette builder and encoder only read these bytes.
  auto* input = const_cast<std::uint8_t*>(pixels);
  check_sixel(sixel_dither_initialize(dither.get(), input, width, height,
      SIXEL_PIXELFORMAT_RGB888, SIXEL_LARGE_AUTO, SIXEL_REP_AUTO, SIXEL_QUALITY_AUTO),
      "initialize SIXEL palette");
  sixel_dither_set_diffusion_type(dither.get(), SIXEL_DIFFUSE_NONE);

  Output state{sink, height < 6, {}};
  sixel_output_t* raw_output = nullptr;
  check_sixel(sixel_output_new(&raw_output, Output::write, &state, nullptr), "create SIXEL output");
  const std::unique_ptr<sixel_output_t, decltype(&sixel_output_unref)>
      output{raw_output, sixel_output_unref};
  const auto status = sixel_encode(input, width, height, channels, dither.get(), output.get());
  if (state.failure) std::rethrow_exception(state.failure);
  check_sixel(status, "encode SIXEL picture");
  if (!sink.flush()) throw std::runtime_error("could not flush SIXEL output");
}

}
