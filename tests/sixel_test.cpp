#include "sixel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr int width = 8;
constexpr int height = 4;
using Color = std::array<int, 3>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Independent decoder for the 7-bit RGB SIXEL used by these raster tests.
class RasterReader {
public:
  explicit RasterReader(std::string_view bytes, int raster_height = height)
      : bytes_(bytes), height_(raster_height), pixels_(width * raster_height) {}

  void check_picture() {
    require(bytes_.starts_with("\033P") && bytes_.ends_with("\033\\"),
            "missing 7-bit DCS framing");
    bytes_.remove_prefix(2);
    bytes_.remove_suffix(2);
    const auto header = bytes_.find_first_not_of("0123456789;");
    require(header != bytes_.npos && bytes_[header] == 'q', "expected SIXEL DCS");
    bytes_.remove_prefix(header + 1);
    expect('"');
    require(number() > 0, "invalid raster aspect");
    expect(';');
    require(number() > 0, "invalid raster aspect");
    expect(';');
    require(number() == width, "unexpected raster width");
    expect(';');
    require(number() == height_, "unexpected raster height");

    while (!bytes_.empty()) {
      const char command = take();
      switch (command) {
      case '#': palette_command(); break;
      case '$': x_ = 0; break;
      case '-': x_ = 0; y_ += 6; break;
      case '!': {
        const int count = number();
        draw(take(), count);
        break;
      }
      default: draw(command, 1); break;
      }
    }
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto& color = pixels_[y * width + x];
        require(color.has_value(), "missing raster pixel");
        // The encoder quantizes to 5-bit palette samples, and SIXEL RGB
        // palette components are then specified in percent.
        constexpr int tolerance = 8;
        if (x < width / 2) {
          require((*color)[0] >= 255 - tolerance && (*color)[1] == 0 && (*color)[2] == 0,
                  "expected red on the left");
        } else {
          require((*color)[0] == 0 && (*color)[1] == 0 && (*color)[2] >= 255 - tolerance,
                  "expected blue on the right");
        }
      }
    }
  }

private:
  char take() {
    require(!bytes_.empty(), "truncated SIXEL");
    const char byte = bytes_.front();
    bytes_.remove_prefix(1);
    return byte;
  }

  void expect(char expected) {
    require(take() == expected, "unexpected SIXEL delimiter");
  }

  int number() {
    int value = 0;
    const auto [end, error] = std::from_chars(bytes_.data(), bytes_.data() + bytes_.size(), value);
    require(error == std::errc{} && value >= 0, "invalid SIXEL decimal parameter");
    bytes_.remove_prefix(end - bytes_.data());
    return value;
  }

  void palette_command() {
    color_ = number();
    require(color_ < static_cast<int>(palette_.size()), "out-of-range palette index");
    if (!bytes_.starts_with(';')) return;
    expect(';');
    require(number() == 2, "expected RGB palette definition");
    Color color{};
    for (auto& channel : color) {
      expect(';');
      const auto percent = number();
      require(percent <= 100, "out-of-range palette component");
      channel = (percent * 255 + 50) / 100;
    }
    palette_[color_] = color;
  }

  void draw(char sixel, int count) {
    require(sixel >= '?' && sixel <= '~', "unexpected SIXEL data byte");
    require(count > 0 && count <= width - x_, "SIXEL data extends outside the raster");
    const int bits = sixel - '?';
    for (int row = 0; row < 6; ++row) {
      if ((bits & (1 << row)) == 0) continue;
      require(y_ + row < height_, "SIXEL data extends past the raster height");
      require(palette_[color_].has_value(), "undefined palette entry");
      for (int column = x_; column < x_ + count; ++column) {
        pixels_[(y_ + row) * width + column] = palette_[color_];
      }
    }
    x_ += count;
  }

  std::string_view bytes_;
  int height_;
  std::array<std::optional<Color>, 256> palette_{};
  std::vector<std::optional<Color>> pixels_;
  int color_ = 0;
  int x_ = 0;
  int y_ = 0;
};

std::vector<std::uint8_t> two_colors(std::size_t stride, int raster_height = height) {
  // Give padding a distinct color and omit padding after the last visible row.
  std::vector<std::uint8_t> bytes((raster_height - 1) * stride + width * 3, 193);
  for (int y = 0; y < raster_height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto* pixel = bytes.data() + y * stride + x * 3;
      pixel[0] = x < width / 2 ? 255 : 0;
      pixel[1] = 0;
      pixel[2] = x < width / 2 ? 0 : 255;
    }
  }
  return bytes;
}

void expect_failure(si::RgbView image, const si::SixelSink& sink, std::string_view expected) {
  try {
    si::encode_sixel(image, sink);
  } catch (const std::runtime_error& failure) {
    require(std::string_view{failure.what()}.find(expected) != std::string_view::npos,
            "unexpected encoder failure diagnostic");
    return;
  }
  throw std::runtime_error("expected the encoder to report failure");
}

void exercise_small_picture(std::string_view kind) {
  const std::size_t raster_height = kind == "multiple_bands" ? 8 : height;
  const std::size_t stride = width * 3 + (kind == "padded" ? 5 : 0);
  const auto pixels = two_colors(stride, static_cast<int>(raster_height));
  std::string output;
  int writes = 0;
  int flushes = 0;
  const si::SixelSink sink{
      [&](std::span<const char> bytes) {
        ++writes;
        const auto written = kind == "short_write" ? std::min<std::size_t>(2, bytes.size()) : bytes.size();
        output.append(bytes.data(), written);
        return written;
      },
      [&] { ++flushes; return kind != "flush_failure"; }};
  const si::RgbView image{pixels, width, raster_height, stride};
  if (kind == "flush_failure") expect_failure(image, sink, "flush SIXEL");
  else si::encode_sixel(image, sink);
  require(flushes == 1, "expected one final flush");
  if (kind == "short_write") require(writes > 1, "short writes were not exercised");
  RasterReader{output, static_cast<int>(raster_height)}.check_picture();
  std::cout << kind << " produced a complete 8x" << raster_height << " raster with the expected colors\n";
}

void midframe_failure() {
  constexpr std::size_t size = 256;
  std::vector<std::uint8_t> pixels(size * size * 3);
  std::uint32_t seed = 5789;
  for (auto& pixel : pixels) {
    seed = seed * 1664525u + 1013904223u;
    pixel = static_cast<std::uint8_t>(seed >> 24);
  }
  const si::RgbView image{pixels, size, size, size * 3};
  int chunks = 0;
  si::encode_sixel(image, {[&](std::span<const char> bytes) { ++chunks; return bytes.size(); },
                           [] { return true; }});
  require(chunks > 1, "fixture must generate several libsixel write callbacks");

  int writes = 0;
  int flushes = 0;
  std::string prefix;
  expect_failure(image, {[&](std::span<const char> bytes) -> std::size_t {
    if (++writes != 1) return 0;
    const auto count = std::min<std::size_t>(11, bytes.size());
    prefix.append(bytes.data(), count);
    return count;
  }, [&] { ++flushes; return true; }}, "write SIXEL");
  require(prefix.size() == 11 && prefix.starts_with("\033P"), "no bytes were accepted before failure");
  require(writes == 2 && flushes == 0, "sink was called after a permanent write failure");
  std::cout << "latched mid-frame failure in a stream with " << chunks << " callbacks\n";
}

void bad_buffer() {
  const auto pixels = two_colors(width * 3);
  int calls = 0;
  const si::SixelSink sink{
      [&](std::span<const char> bytes) { ++calls; return bytes.size(); },
      [&] { ++calls; return true; }};
  expect_failure({pixels, 0, height, width * 3}, sink, "dimensions");
  expect_failure({pixels, std::numeric_limits<std::size_t>::max(), height, width * 3},
                 sink, "dimensions");
  expect_failure({pixels, 100000, 100000, 300000}, sink, "byte count");
  expect_failure({pixels, width, height, width * 3 - 1}, sink, "buffer");
  expect_failure({pixels, width, height, std::numeric_limits<std::size_t>::max()}, sink, "buffer");
  expect_failure({std::span{pixels}.first(pixels.size() - 1), width, height, width * 3}, sink, "buffer");
  require(calls == 0, "invalid input reached the output sink");
}

#ifndef _WIN32
void closed_pipe(const char* program, const char* fixture) {
  int output[2];
  int errors[2];
  require(pipe(output) == 0, "could not create output pipe");
  close(output[0]);
  require(pipe(errors) == 0, "could not create diagnostic pipe");
  const auto child = fork();
  require(child >= 0, "could not fork si");
  if (child == 0) {
    close(errors[0]);
    if (dup2(output[1], STDOUT_FILENO) < 0 || dup2(errors[1], STDERR_FILENO) < 0) _exit(127);
    close(output[1]);
    close(errors[1]);
    execl(program, program, fixture, static_cast<char*>(nullptr));
    _exit(127);
  }
  close(output[1]);
  close(errors[1]);
  std::string diagnostic;
  char buffer[256];
  for (;;) {
    const auto count = read(errors[0], buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0 && errno == EINTR) continue;
    require(count > 0, "could not read child's diagnostic");
    diagnostic.append(buffer, static_cast<std::size_t>(count));
  }
  close(errors[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    require(errno == EINTR, "could not wait for si");
  }
  require(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "closed-pipe failure should exit normally with status 1");
  require(diagnostic.starts_with("si: ") && diagnostic.find("SIXEL") != std::string::npos,
          "closed pipe should produce a SIXEL output diagnostic");
  std::cout << "closed pipe exited 1 with diagnostic " << diagnostic;
}
#endif
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  try {
    const std::string_view kind = argv[1];
    if (kind == "raster" && argc == 3) {
      std::ifstream file(argv[2], std::ios::binary);
      require(file.is_open(), "could not open captured SIXEL output");
      const std::string output{std::istreambuf_iterator<char>{file}, {}};
      RasterReader{output}.check_picture();
      std::cout << "one 7-bit SIXEL picture, 8x4, all 32 pixels matched\n";
    } else if (kind == "padded" || kind == "short_write" || kind == "flush_failure" ||
               kind == "multiple_bands") {
      exercise_small_picture(kind);
    } else if (kind == "midframe_failure") {
      midframe_failure();
    } else if (kind == "bad_buffer") {
      bad_buffer();
#ifndef _WIN32
    } else if (kind == "closed_pipe" && argc == 4) {
      closed_pipe(argv[2], argv[3]);
#endif
    } else {
      return 2;
    }
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
