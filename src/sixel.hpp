#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace si {

const char* sixel_version();

struct RgbView {
  std::span<const std::uint8_t> bytes;
  std::size_t width;
  std::size_t height;
  std::size_t stride;
};

struct SixelSink {
  // Return the number of bytes accepted, or zero if the write cannot continue.
  std::function<std::size_t(std::span<const char>)> write;
  std::function<bool()> flush;
};

// Validate the encoder's dimension and signed byte-count limits before allocating.
std::size_t sixel_rgb_size(std::size_t width, std::size_t height);
void encode_sixel(RgbView image, const SixelSink& sink);

}
