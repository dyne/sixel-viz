#include "media.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace {
using Color = std::array<int, 3>;
constexpr Color red{255, 0, 0};
constexpr Color green{0, 255, 0};
constexpr Color blue{0, 0, 255};
constexpr Color white{255, 255, 255};
constexpr Color black{0, 0, 0};
constexpr Color yellow{255, 255, 0};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void check_dimensions(const si::DisplayFrame& frame, int width, int height) {
  require(frame.width == width && frame.height == height, "unexpected frame dimensions");
  require(frame.rgb.size() == static_cast<std::size_t>(width * height * 3),
          "frame does not contain exactly enough packed RGB bytes");
}

void check_pixel(const si::DisplayFrame& frame, int x, int y, Color expected, int tolerance = 0) {
  const auto* actual = frame.rgb.data() + (y * frame.width + x) * 3;
  for (int channel = 0; channel < 3; ++channel) {
    if (std::abs(actual[channel] - expected[channel]) > tolerance) {
      throw std::runtime_error("unexpected pixel at (" + std::to_string(x) + "," +
          std::to_string(y) + "): (" + std::to_string(actual[0]) + "," +
          std::to_string(actual[1]) + "," + std::to_string(actual[2]) + ")");
    }
  }
}

void check_time(std::optional<std::int64_t> timestamp, AVRational time_base,
                int milliseconds, const char* message) {
  require(timestamp && time_base.num > 0 && time_base.den > 0 &&
              av_compare_ts(*timestamp, time_base, milliseconds, {1, 1000}) == 0,
          message);
}

void check_still(si::MediaDecoder& decoder, std::string_view kind) {
  const auto picture = decoder.next_frame();
  require(picture.has_value(), "expected one still frame");
  const auto& frame = *picture;
  require(!decoder.next_frame(), "still input produced a second frame");
  require(frame.sample_aspect_ratio.num == 1 && frame.sample_aspect_ratio.den == 1,
          "expected square pixels");

  if (kind == "jpeg" || kind == "mirrored") {
    check_dimensions(frame, 12, 16);
    const bool rotated = kind == "jpeg";
    require(frame.orientation.swap_axes && frame.orientation.flip_x == rotated &&
                !frame.orientation.flip_y, "unexpected JPEG orientation");
    constexpr int tolerance = 12;
    check_pixel(frame, rotated ? 9 : 2, 2, red, tolerance);
    check_pixel(frame, rotated ? 9 : 2, 13, green, tolerance);
    check_pixel(frame, rotated ? 2 : 9, 2, blue, tolerance);
    check_pixel(frame, rotated ? 2 : 9, 13, white, tolerance);
    std::cout << "one 12x16 frame, EXIF orientation and four corners verified\n";
    return;
  }

  check_dimensions(frame, 8, 4);
  require(!frame.orientation.swap_axes && !frame.orientation.flip_x &&
              !frame.orientation.flip_y, "unexpected transform on a plain still");
  if (kind == "png") {
    check_pixel(frame, 1, 2, {255, 32, 64});
    check_pixel(frame, 3, 2, {120, 63, 10});
    check_pixel(frame, 5, 2, black);
    check_pixel(frame, 7, 2, blue);
  } else if (kind == "svg_alpha") {
    check_pixel(frame, 1, 1, black);
    check_pixel(frame, 2, 1, {64, 32, 0}, 1);
    check_pixel(frame, 3, 1, {128, 64, 0}, 1);
    check_pixel(frame, 6, 1, {64, 32, 0}, 1);
  } else {
    check_pixel(frame, 1, 2, red);
    check_pixel(frame, 6, 2, blue);
  }
  if (kind == "gif") {
    check_time(frame.pts, frame.time_base, 0, "GIF still should start at time zero");
    check_time(frame.duration, frame.time_base, 100, "GIF frame should last 100 ms");
  }
  std::cout << "one 8x4 frame and reference pixels verified\n";
}

void check_bars(const si::DisplayFrame& frame, bool covered, int index) {
  check_dimensions(frame, covered ? 16 : 32, covered ? 32 : 16);
  require(frame.orientation.swap_axes == covered && !frame.orientation.flip_x &&
              frame.orientation.flip_y == covered, "unexpected video display matrix");
  require(frame.sample_aspect_ratio.num == (covered ? 1 : 2) &&
              frame.sample_aspect_ratio.den == (covered ? 2 : 1),
          "video sample aspect ratio was not retained in the oriented raster");
  require(frame.source_color_range == AVCOL_RANGE_MPEG, "expected limited YUV range");
  require(frame.source_color_space == AVCOL_SPC_BT709, "expected BT.709 color matrix");
  check_time(frame.pts, frame.time_base, index * 125, "unexpected video presentation timestamp");
  check_time(frame.duration, frame.time_base, 125, "unexpected video frame duration");

  constexpr int tolerance = 6;
  const auto sample = [&](int x, int y, Color color) {
    check_pixel(frame, covered ? y : x, covered ? 31 - x : y, color, tolerance);
  };
  sample(4, 12, red);
  sample(12, 12, green);
  sample(20, 12, blue);
  sample(28, 12, white);
  constexpr std::array markers{red, green, blue, white, black, yellow};
  sample(24, 4, markers.at(index));
}

void check_video(si::MediaDecoder& decoder, bool covered) {
  const auto count = decoder.declared_frame_count();
  require(covered ? count == 6 : !count, "unexpected declared video frame count");
  require(av_cmp_q(decoder.frame_rate(), {8, 1}) == 0, "expected 8 fps");
  const auto first = decoder.next_frame();
  auto next = decoder.next_frame();
  require(first && next, "lookahead must find two frames regardless of declared count");
  check_bars(*first, covered, 0);
  for (int index = 1; index < 6; ++index) {
    require(next.has_value(), "a reordered or delayed frame is missing");
    check_bars(*next, covered, index);
    next = decoder.next_frame();
  }
  require(!next && !decoder.next_frame(), "video did not end after six frames");
  check_bars(*first, covered, 0);
  std::cout << "six ordered frames with 125 ms spacing and BT.709 pixels verified, "
            << (covered ? "ordinary rotated video selected over attached cover art"
                        : "no declared frame count") << '\n';
}

bool optional_format_available(std::string_view kind) {
  if (kind == "ppm") return av_find_input_format("ppm_pipe") && avcodec_find_decoder(AV_CODEC_ID_PPM);
  if (kind == "qoi") return av_find_input_format("qoi_pipe") && avcodec_find_decoder(AV_CODEC_ID_QOI);
  if (kind == "jxl") return av_find_input_format("jpegxl_pipe") && avcodec_find_decoder(AV_CODEC_ID_JPEGXL);
  return true;
}
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string_view kind = argv[1];
  if (!optional_format_available(kind)) {
    std::cout << "optional FFmpeg decoder or demuxer unavailable for " << kind << '\n';
    return 77;
  }
  try {
    si::MediaDecoder decoder(argv[2]);
    if (kind == "limited_yuv" || kind == "covered") check_video(decoder, kind == "covered");
    else check_still(decoder, kind);
  } catch (const std::exception& error) {
    std::cerr << argv[2] << ": " << error.what() << '\n';
    return 1;
  }
}
