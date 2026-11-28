#include "exif.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/intreadwrite.h>
}

namespace si {
namespace {

using Bytes = std::span<const char>;

std::optional<std::string> original_date(Bytes tiff) {
  if (std::string_view{tiff.data(), tiff.size()}.starts_with(std::string_view{"Exif\0\0", 6})) {
    tiff = tiff.subspan(6);
  }
  if (tiff.size() < 8) return std::nullopt;
  const std::string_view endian{tiff.data(), 2};
  if (endian != "II" && endian != "MM") return std::nullopt;
  const bool little = endian == "II";
  const auto short_at = [&](std::size_t at) {
    return little ? AV_RL16(tiff.data() + at) : AV_RB16(tiff.data() + at);
  };
  const auto long_at = [&](std::size_t at) {
    return little ? AV_RL32(tiff.data() + at) : AV_RB32(tiff.data() + at);
  };
  if (short_at(2) != 42) return std::nullopt;

  const auto tag_offset = [&](std::uint32_t directory, std::uint16_t tag)
      -> std::optional<std::size_t> {
    if (directory > tiff.size() - 2) return std::nullopt;
    const auto count = short_at(directory);
    std::size_t offset = directory + 2;
    if (count > (tiff.size() - offset) / 12) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i, offset += 12) {
      if (short_at(offset) == tag) return offset;
    }
    return std::nullopt;
  };

  const auto exif_pointer = tag_offset(long_at(4), 0x8769);
  if (!exif_pointer || short_at(*exif_pointer + 2) != 4 || long_at(*exif_pointer + 4) != 1) {
    return std::nullopt;
  }
  const auto capture = tag_offset(long_at(*exif_pointer + 8), 0x9003);
  if (!capture || short_at(*capture + 2) != 2 || long_at(*capture + 4) < 20) return std::nullopt;
  const auto date_start = long_at(*capture + 8);
  if (date_start > tiff.size() || tiff.size() - date_start < 20 ||
      tiff[date_start + 19] != '\0') {
    return std::nullopt;
  }
  return std::string{tiff.data() + date_start, 19};
}

}

std::optional<std::string> embedded_exif_date(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto size = static_cast<std::streamoff>(file.tellg());
  if (size < 12 || !file.seekg(0)) return std::nullopt;
  std::array<char, 12> header{};
  if (!file.read(header.data(), header.size())) return std::nullopt;

  const std::string_view signature{header.data(), header.size()};
  const bool png = signature.starts_with("\x89PNG\r\n\x1a\n");
  const bool webp = signature.starts_with("RIFF") && signature.ends_with("WEBP");
  if (!png && !webp) return std::nullopt;
  if (png && !file.seekg(8)) return std::nullopt;
  const auto limit = webp ? std::min(size, std::streamoff{AV_RL32(header.data() + 4)} + 8) : size;
  auto remaining = limit - (png ? 8 : 12);

  std::array<char, 8> chunk{};
  while (remaining >= 8 && file.read(chunk.data(), chunk.size())) {
    const auto length = png ? AV_RB32(chunk.data()) : AV_RL32(chunk.data() + 4);
    const std::string_view type{chunk.data() + (png ? 4 : 0), 4};
    const auto bytes = std::streamoff{length} + (png ? 4 : length % 2);  // CRC or RIFF padding.
    if (bytes > remaining - 8 || (png && type == "IEND")) return std::nullopt;
    if (type == (png ? "eXIf" : "EXIF")) {
      // Capture timestamps occur in small EXIF directories. Cap metadata storage
      // independently of the size claimed by the surrounding image.
      if (length < 8 || length > 1024 * 1024) return std::nullopt;
      std::vector<char> profile(length);
      if (!file.read(profile.data(), length)) return std::nullopt;
      return original_date(profile);
    }
    remaining -= 8 + bytes;
    if (!file.seekg(bytes, std::ios::cur)) break;
  }
  return std::nullopt;
}

}
