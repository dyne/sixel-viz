#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace si {

// Read DateTimeOriginal from image containers whose decoder may omit EXIF.
std::optional<std::string> embedded_exif_date(const std::filesystem::path& path);

}
