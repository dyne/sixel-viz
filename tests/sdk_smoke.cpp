#include <iostream>

#include <sixel.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

int main() {
  sixel_encoder_t* encoder = nullptr;
  if (SIXEL_FAILED(sixel_encoder_new(&encoder, nullptr))) {
    std::cerr << "Could not initialize the libsixel encoder\n";
    return 1;
  }
  sixel_encoder_unref(encoder);

  std::cout << "libsixel headers " LIBSIXEL_VERSION "\n";
  std::cout << "FFmpeg runtime " << av_version_info() << '\n';
  std::cout << "FFmpeg configuration " << avcodec_configuration() << '\n';
  if (avformat_version() != LIBAVFORMAT_VERSION_INT ||
      avcodec_version() != LIBAVCODEC_VERSION_INT ||
      avutil_version() != LIBAVUTIL_VERSION_INT ||
      swscale_version() != LIBSWSCALE_VERSION_INT) {
    std::cerr << "FFmpeg headers and runtime versions differ\n";
    return 1;
  }
}
