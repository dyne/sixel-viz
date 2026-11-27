#include "media.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace si {
namespace {

void check(int status, const char* operation) {
  if (status < 0) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(status, message.data(), message.size());
    throw std::runtime_error(std::string{operation} + ": " + message.data());
  }
}

template <auto free_resource>
struct FreeAv {
  void operator()(auto* resource) const { free_resource(&resource); }
};

void free_io(AVIOContext* io) {
  if (io) {
    // Probing may replace the original buffer.
    av_freep(&io->buffer);
    avio_context_free(&io);
  }
}

struct LocalFile {
  explicit LocalFile(const std::filesystem::path& path)
      : stream(path, std::ios::binary | std::ios::ate), size(stream.tellg()) {
    if (size < 0 || !stream.seekg(0)) {
      throw std::runtime_error("cannot read local file");
    }
  }

  static int read(void* opaque, std::uint8_t* buffer, int bytes) {
    auto& file = static_cast<LocalFile*>(opaque)->stream;
    file.read(reinterpret_cast<char*>(buffer), bytes);
    if (file.bad()) return AVERROR(EIO);
    if (file.gcount() > 0) return static_cast<int>(file.gcount());
    return file.eof() ? AVERROR_EOF : AVERROR(EIO);
  }

  static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
    auto& file = *static_cast<LocalFile*>(opaque);
    std::ios::seekdir direction;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return file.size;
    case SEEK_SET: direction = std::ios::beg; break;
    case SEEK_CUR: direction = std::ios::cur; break;
    case SEEK_END: direction = std::ios::end; break;
    default: return AVERROR(EINVAL);
    }
    file.stream.clear();
    file.stream.seekg(offset, direction);
    return file.stream ? static_cast<std::int64_t>(file.stream.tellg()) : AVERROR(EIO);
  }

  std::ifstream stream;
  std::int64_t size;
};

using Input = std::unique_ptr<AVFormatContext, FreeAv<avformat_close_input>>;

Input open_input(AVIOContext* io) {
  // Supplying no filename keeps the probe independent of the extension and
  // gives sequence demuxers no pattern or directory to expand.
  const AVInputFormat* demuxer = nullptr;
  check(av_probe_input_buffer2(io, &demuxer, "", nullptr, 0, 0), "probe media format");
  if (demuxer->flags & AVFMT_NOFILE) {
    throw std::runtime_error("media format requires opening additional input");
  }

  auto* format = avformat_alloc_context();
  if (!format) throw std::bad_alloc{};
  Input input{format};
  format->pb = io;
  format->flags |= AVFMT_FLAG_CUSTOM_IO;
  format->io_open = [](AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EPERM);
  };
  // Demuxers must use the supplied I/O context, including when opening a
  // protocol directly instead of through io_open.
  check(av_opt_set(format, "protocol_whitelist", "", 0), "restrict media input");

  AVDictionary* options = nullptr;
  check(av_dict_set(&options, "ignore_loop", "1", 0), "disable media looping");
  format = input.release();
  const auto status = avformat_open_input(&format, "", demuxer, &options);
  av_dict_free(&options);
  input.reset(format);
  check(status, "open media");
  check(avformat_find_stream_info(input.get(), nullptr), "read media streams");
  return input;
}

int video_stream(const AVFormatContext& input) {
  int cover = -1;
  for (unsigned index = 0; index < input.nb_streams; ++index) {
    const auto* stream = input.streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
    if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) return stream->index;
    if (cover < 0) cover = stream->index;
  }
  if (cover >= 0) return cover;
  throw std::runtime_error("no video or image stream");
}

DisplayOrientation display_orientation(const AVFrame& frame, const AVStream& stream) {
  const auto* frame_side = av_frame_get_side_data(&frame, AV_FRAME_DATA_DISPLAYMATRIX);
  const auto* stream_side = av_packet_side_data_get(
      stream.codecpar->coded_side_data, stream.codecpar->nb_coded_side_data,
      AV_PKT_DATA_DISPLAYMATRIX);
  std::array<std::int32_t, 9> matrix{};
  if (frame_side && frame_side->size >= sizeof(matrix)) {
    std::memcpy(matrix.data(), frame_side->data, sizeof(matrix));
  } else if (stream_side && stream_side->size >= sizeof(matrix)) {
    std::memcpy(matrix.data(), stream_side->data, sizeof(matrix));
  } else {
    return {};
  }

  // Use the direction of each source axis in the display matrix. This keeps
  // mirrors as well as quarter turns. The oriented raster has its own origin.
  const auto zero = [](std::int32_t value) { return value >= -1 && value <= 1; };
  const auto a = matrix[0], b = matrix[1], c = matrix[3], d = matrix[4];
  if (matrix[2] == 0 && matrix[5] == 0 && matrix[8] > 0) {
    if (zero(b) && zero(c) && !zero(a) && !zero(d)) return {false, a < 0, d < 0};
    if (zero(a) && zero(d) && !zero(b) && !zero(c)) return {true, c < 0, b < 0};
  }
  throw std::runtime_error("unsupported display transform");
}

int scale_coefficients(AVColorSpace space) {
  switch (space) {
  case AVCOL_SPC_BT709: return SWS_CS_ITU709;
  case AVCOL_SPC_FCC: return SWS_CS_FCC;
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
  case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
  case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
  default: return SWS_CS_DEFAULT;
  }
}

bool full_range(const AVPixFmtDescriptor& format, AVColorRange range) {
  if (format.flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) return true;
  if (range != AVCOL_RANGE_UNSPECIFIED) return range == AVCOL_RANGE_JPEG;
  // JPEG-range pixel formats may carry the range in the format itself.
  return std::string_view{format.name}.starts_with("yuvj") || format.nb_components <= 2;
}

}

struct MediaDecoder::State {
  explicit State(const std::filesystem::path& path) : file(path) {
    constexpr int buffer_size = 32 * 1024;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(buffer_size));
    if (!buffer) throw std::bad_alloc{};
    io.reset(avio_alloc_context(buffer, buffer_size, 0, &file,
                                LocalFile::read, nullptr, LocalFile::seek));
    if (!io) {
      av_free(buffer);
      throw std::bad_alloc{};
    }
    input = open_input(io.get());
    stream_index = video_stream(*input);
    const auto& stream = *input->streams[stream_index];
    const auto* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
    if (!decoder) throw std::runtime_error("no decoder for the selected media stream");
    codec.reset(avcodec_alloc_context3(decoder));
    packet.reset(av_packet_alloc());
    frame.reset(av_frame_alloc());
    converted.reset(av_frame_alloc());
    if (!codec || !packet || !frame || !converted) throw std::bad_alloc{};
    check(avcodec_parameters_to_context(codec.get(), stream.codecpar), "read codec parameters");
    codec->pkt_timebase = stream.time_base;
    check(avcodec_open2(codec.get(), decoder, nullptr), "open media decoder");
    // FFmpeg's librsvg decoder renders into Cairo's premultiplied ARGB32.
    premultiplied = std::string_view{decoder->name} == "librsvg";
  }

  DisplayFrame convert() {
    const auto& stream = *input->streams[stream_index];
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor) throw std::runtime_error("unknown decoded pixel format");
    const int width = frame->width;
    const int height = frame->height;
    check(av_image_check_size(width, height, 0, nullptr), "validate decoded dimensions");

    DisplayFrame result;
    result.orientation = display_orientation(*frame, stream);
    const auto [swap_axes, flip_x, flip_y] = result.orientation;
    result.width = swap_axes ? height : width;
    result.height = swap_axes ? width : height;
    auto sar = av_guess_sample_aspect_ratio(input.get(), input->streams[stream_index], frame.get());
    if (sar.num > 0 && sar.den > 0) {
      result.sample_aspect_ratio = swap_axes ? AVRational{sar.den, sar.num} : sar;
    }
    result.source_color_range = frame->color_range == AVCOL_RANGE_UNSPECIFIED
        ? stream.codecpar->color_range : frame->color_range;
    result.source_color_space = frame->colorspace == AVCOL_SPC_UNSPECIFIED
        ? stream.codecpar->color_space : frame->colorspace;
    result.time_base = stream.time_base;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
      result.pts = frame->best_effort_timestamp;
    }
    if (frame->duration > 0) result.duration = frame->duration;

    scaler.reset(sws_getCachedContext(scaler.release(), width, height, format,
        width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) throw std::runtime_error("could not prepare RGB conversion");
    const auto* coefficients = sws_getCoefficients(scale_coefficients(result.source_color_space));
    check(sws_setColorspaceDetails(scaler.get(), coefficients,
        full_range(*descriptor, result.source_color_range), coefficients, 1,
        0, 1 << 16, 1 << 16), "set media color range and matrix");

    if (converted->width != width || converted->height != height) {
      av_frame_unref(converted.get());
      converted->width = width;
      converted->height = height;
      converted->format = AV_PIX_FMT_RGBA;
      check(av_frame_get_buffer(converted.get(), 0), "allocate RGB conversion frame");
    }
    if (sws_scale(scaler.get(), frame->data, frame->linesize, 0, height,
                  converted->data, converted->linesize) != height) {
      throw std::runtime_error("could not convert every decoded pixel row");
    }

    const auto byte_count = av_image_get_buffer_size(AV_PIX_FMT_RGB24, result.width, result.height, 1);
    check(byte_count, "validate RGB frame size");
    result.rgb.resize(static_cast<std::size_t>(byte_count));
    for (int y = 0; y < height; ++y) {
      const auto* row = converted->data[0] + static_cast<std::ptrdiff_t>(y) * converted->linesize[0];
      for (int x = 0; x < width; ++x) {
        const int oriented_x = swap_axes ? y : x;
        const int oriented_y = swap_axes ? x : y;
        const int dx = flip_x ? result.width - 1 - oriented_x : oriented_x;
        const int dy = flip_y ? result.height - 1 - oriented_y : oriented_y;
        const auto* source = row + static_cast<std::ptrdiff_t>(x) * 4;
        auto* pixel = result.rgb.data() + (dy * result.width + dx) * 3;
        for (int channel = 0; channel < 3; ++channel) {
          pixel[channel] = premultiplied ? source[channel] :
              static_cast<std::uint8_t>((source[channel] * source[3] + 127) / 255);
        }
      }
    }
    return result;
  }

  bool read_packet() {
    while (true) {
      const int status = av_read_frame(input.get(), packet.get());
      if (status == AVERROR_EOF) return false;
      check(status, "read media packet");
      if (packet->stream_index == stream_index) return true;
      av_packet_unref(packet.get());
    }
  }

  std::optional<DisplayFrame> next_frame() {
    while (true) {
      const int received = avcodec_receive_frame(codec.get(), frame.get());
      if (received == AVERROR_EOF) return std::nullopt;
      if (received != AVERROR(EAGAIN)) {
        check(received, "decode media frame");
        if (frame->flags & AV_FRAME_FLAG_CORRUPT) {
          throw std::runtime_error("decoded media frame is corrupt");
        }
        return convert();
      }

      if (draining) throw std::runtime_error("media decoder did not finish draining");
      if (read_packet()) {
        // receive just returned EAGAIN, so the send/receive contract guarantees
        // that this packet can be submitted without losing pending output.
        const int sent = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        check(sent, "submit media packet");
      } else {
        check(avcodec_send_packet(codec.get(), nullptr), "drain media decoder");
        draining = true;
      }
    }
  }

  LocalFile file;
  std::unique_ptr<AVIOContext, decltype(&free_io)> io{nullptr, free_io};
  Input input;
  std::unique_ptr<AVCodecContext, FreeAv<avcodec_free_context>> codec;
  std::unique_ptr<AVPacket, FreeAv<av_packet_free>> packet;
  std::unique_ptr<AVFrame, FreeAv<av_frame_free>> frame;
  std::unique_ptr<AVFrame, FreeAv<av_frame_free>> converted;
  std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler{nullptr, sws_freeContext};
  int stream_index = -1;
  bool premultiplied = false;
  bool draining = false;
};

MediaDecoder::MediaDecoder(const std::filesystem::path& path)
    : state_(std::make_unique<State>(path)) {}
MediaDecoder::~MediaDecoder() = default;

std::optional<DisplayFrame> MediaDecoder::next_frame() {
  return state_->next_frame();
}

std::optional<std::int64_t> MediaDecoder::declared_frame_count() const {
  const auto count = state_->input->streams[state_->stream_index]->nb_frames;
  return count > 0 ? std::optional{count} : std::nullopt;
}

AVRational MediaDecoder::frame_rate() const {
  auto* input = state_->input.get();
  return av_guess_frame_rate(input, input->streams[state_->stream_index], nullptr);
}

}
