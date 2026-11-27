#include "fit.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using Color = std::array<int, 3>;
constexpr si::Dimensions viewport{640, 368};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void check_size(si::Dimensions size, int width, int height) {
  require(size.width == width && size.height == height, "unexpected fitted dimensions");
}

void check_image(si::RgbView image, std::size_t width, std::size_t height) {
  require(image.width == width && image.height == height, "unexpected fitted RGB dimensions");
  require(image.stride == width * 3 && image.bytes.size() == image.stride * height,
          "fitted RGB must be packed");
}

void check_pixel(si::RgbView image, std::size_t x, std::size_t y, Color color, int tolerance = 0) {
  for (std::size_t channel = 0; channel < color.size(); ++channel) {
    const auto value = image.bytes[y * image.stride + x * color.size() + channel];
    require(std::abs(value - color[channel]) <= tolerance, "unexpected fitted RGB pixel");
  }
}

void check_dimensions() {
  check_size(si::fit_dimensions({1920, 1080}, {1, 1}, viewport), 640, 360);
  check_size(si::fit_dimensions({16, 9}, {1, 1}, viewport), 16, 9);
  check_size(si::fit_dimensions({1080, 1920}, {1, 1}, viewport), 207, 368);
  check_size(si::fit_dimensions({960, 1080}, {2, 1}, viewport), 640, 360);
  check_size(si::fit_dimensions({32, 16}, {2, 1}, std::nullopt), 64, 16);
}

void check_rounding() {
  check_size(si::fit_dimensions({13, 10}, {1, 2}, std::nullopt), 7, 10);
  check_size(si::fit_dimensions({1, 10}, {1, 10}, std::nullopt), 1, 10);
  check_size(si::fit_dimensions({13, 10}, {1, 2}, si::Dimensions{6, 12}), 6, 9);
  check_size(si::fit_dimensions({1920, 1080}, {1, 1}, si::Dimensions{1, 368}), 1, 1);
  check_size(si::fit_dimensions({6, 8}, {0, 0}, viewport), 6, 8);
  const int largest = std::numeric_limits<int>::max();
  check_size(si::fit_dimensions({largest, largest}, {largest, largest - 1}, viewport), 368, 368);
  check_size(si::fit_dimensions({largest, largest}, {largest, 1}, viewport), 640, 1);
}

void check_pixels() {
  si::DisplayFrame source;
  source.width = 4;
  source.height = 4;
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      source.rgb.push_back(static_cast<std::uint8_t>(x * 40));
      source.rgb.push_back(static_cast<std::uint8_t>(y * 40));
      source.rgb.push_back(255);
    }
  }
  std::vector<std::uint8_t> output;
  const auto fitted = si::fit_frame(source, si::Dimensions{2, 2}, output);
  check_image(fitted, 2, 2);
  check_pixel(fitted, 0, 0, {40, 40, 255});
  check_pixel(fitted, 1, 0, {120, 40, 255});
  check_pixel(fitted, 0, 1, {40, 120, 255});
  check_pixel(fitted, 1, 1, {120, 120, 255});

  const auto native = si::fit_frame(source, viewport, output);
  check_image(native, 4, 4);
  require(native.bytes.data() == source.rgb.data(), "native size must reuse decoded pixels");
}

void check_rotated(const char* path) {
  si::MediaDecoder decoder(path);
  const auto frame = decoder.next_frame();
  require(frame.has_value(), "rotated fixture must decode");
  std::vector<std::uint8_t> output;
  const auto image = si::fit_frame(*frame, si::Dimensions{8, 8}, output);
  check_image(image, 6, 8);
  check_pixel(image, 5, 1, {255, 0, 0}, 12);
  check_pixel(image, 5, 6, {0, 255, 0}, 12);
  check_pixel(image, 1, 1, {0, 0, 255}, 12);
  check_pixel(image, 1, 6, {255, 255, 255}, 12);
}

void check_range() {
  si::DisplayFrame source;
  source.width = 2;
  source.height = 1;
  source.sample_aspect_ratio = {1000001, 1};
  source.rgb.resize(6);
  std::vector<std::uint8_t> output;
  try {
    si::fit_frame(source, std::nullopt, output);
  } catch (const std::runtime_error&) {
    require(output.empty(), "oversize raster must be rejected before allocation");
    return;
  }
  throw std::runtime_error("oversize raster was accepted by fit");
}

}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string_view test = argv[1];
  try {
    if (test == "dimensions") check_dimensions();
    else if (test == "rounding") check_rounding();
    else if (test == "pixels") check_pixels();
    else if (test == "range") check_range();
    else if (test == "rotated" && argc == 3) check_rotated(argv[2]);
    else return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
