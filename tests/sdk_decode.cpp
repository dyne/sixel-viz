#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr int width = 8;
constexpr int height = 4;
constexpr int channels = 3;

void check(int status, std::string_view operation) {
  if (status < 0) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> detail{};
    av_strerror(status, detail.data(), detail.size());
    throw std::runtime_error(std::string{operation} + ": " + detail.data());
  }
}

template <auto free_resource>
constexpr auto free_av = [](auto* resource) { free_resource(&resource); };

void check_picture(const AVFrame& frame) {
  if (frame.width != width || frame.height != height) {
    throw std::runtime_error("Decoded dimensions were " + std::to_string(frame.width) +
                             "x" + std::to_string(frame.height) + ", expected 8x4");
  }

  const auto format = static_cast<AVPixelFormat>(frame.format);
  const std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler{
      sws_getContext(width, height, format, width, height, AV_PIX_FMT_RGB24,
                     SWS_POINT, nullptr, nullptr, nullptr),
      sws_freeContext};
  if (!scaler) {
    throw std::runtime_error("Could not create RGB conversion context");
  }

  // Give the scaler an aligned stride with room for SIMD writes at the row end.
  constexpr int stride = 64;
  alignas(64) std::array<std::uint8_t, stride * height> rgb{};
  std::uint8_t* destination[] = {rgb.data(), nullptr, nullptr, nullptr};
  int strides[] = {stride, 0, 0, 0};
  if (sws_scale(scaler.get(), frame.data, frame.linesize, 0, height, destination,
                strides) != height) {
    throw std::runtime_error("RGB conversion did not produce every row");
  }

  const auto check_pixel = [&](int x, int y, std::array<int, channels> expected) {
    const auto pixel = rgb.data() + y * stride + x * channels;
    if (pixel[0] != expected[0] || pixel[1] != expected[1] || pixel[2] != expected[2]) {
      throw std::runtime_error("Unexpected RGB at (" + std::to_string(x) + "," +
                               std::to_string(y) + "): " + std::to_string(pixel[0]) +
                               "," + std::to_string(pixel[1]) + "," +
                               std::to_string(pixel[2]));
    }
  };
  check_pixel(1, 2, {255, 0, 0});
  check_pixel(6, 2, {0, 0, 255});
}

void decode(bool svg, const char* path) {
  const auto expected_codec = svg ? AV_CODEC_ID_SVG : AV_CODEC_ID_GIF;
  const char* expected_demuxer = svg ? "svg_pipe" : "gif";
  if (!av_find_input_format(expected_demuxer)) {
    throw std::runtime_error(std::string{"Missing FFmpeg demuxer: "} + expected_demuxer);
  }
  const auto* decoder = avcodec_find_decoder(expected_codec);
  if (!decoder) {
    throw std::runtime_error(std::string{"Missing FFmpeg decoder: "} +
                             avcodec_get_name(expected_codec));
  }

  AVFormatContext* input = nullptr;
  check(avformat_open_input(&input, path, nullptr, nullptr), "Open fixture");
  const std::unique_ptr<AVFormatContext, decltype(free_av<avformat_close_input>)>
      demuxer{input};
  check(avformat_find_stream_info(input, nullptr), "Read streams");
  const int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  check(video, "Find video stream");
  const auto* stream = input->streams[video];
  if (stream->codecpar->codec_id != expected_codec ||
      std::string_view{input->iformat->name} != expected_demuxer) {
    throw std::runtime_error("Content probing selected an unexpected demuxer or codec");
  }

  const std::unique_ptr<AVCodecContext, decltype(free_av<avcodec_free_context>)>
      context{avcodec_alloc_context3(decoder)};
  const std::unique_ptr<AVPacket, decltype(free_av<av_packet_free>)>
      packet{av_packet_alloc()};
  const std::unique_ptr<AVFrame, decltype(free_av<av_frame_free>)>
      frame{av_frame_alloc()};
  if (!context || !packet || !frame) {
    throw std::runtime_error("Could not allocate the FFmpeg decoding state");
  }
  check(avcodec_parameters_to_context(context.get(), stream->codecpar), "Copy codec parameters");
  check(avcodec_open2(context.get(), decoder, nullptr), "Open decoder");
  std::cout << "demuxer=" << input->iformat->name << " decoder=" << decoder->name << '\n';

  int frames = 0;
  const auto receive = [&] {
    int status = 0;
    while ((status = avcodec_receive_frame(context.get(), frame.get())) >= 0) {
      ++frames;
      check_picture(*frame);
      if (!svg && av_rescale_q(frame->duration, stream->time_base, {1, 1000}) != 100) {
        throw std::runtime_error("Expected a GIF frame duration of 100 ms");
      }
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) {
      check(status, "Decode picture");
    }
  };

  int read_status = 0;
  while ((read_status = av_read_frame(input, packet.get())) >= 0) {
    if (packet->stream_index == video) {
      check(avcodec_send_packet(context.get(), packet.get()), "Submit video packet");
      receive();
    }
    av_packet_unref(packet.get());
  }
  if (read_status != AVERROR_EOF) {
    check(read_status, "Read fixture");
  }
  check(avcodec_send_packet(context.get(), nullptr), "Flush decoder");
  receive();
  if (frames != 1) {
    throw std::runtime_error("Decoded " + std::to_string(frames) + " frames, expected one");
  }
  std::cout << "8x4, 1 frame, both interior colors verified\n";
}
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: sdk_decode FORMAT FILE\n";
    return 2;
  }
  try {
    decode(std::string_view{argv[1]} == "svg", argv[2]);
  } catch (const std::runtime_error& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
