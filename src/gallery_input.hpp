#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace si {

struct GalleryFile {
  std::filesystem::path path;
  bool explicit_file;
};

struct InputError {
  std::filesystem::path path;
  std::string message;
};

struct GalleryInputs {
  std::vector<GalleryFile> files;
  std::vector<InputError> errors;
  bool batch;
};

GalleryInputs collect_gallery_inputs(std::span<const std::filesystem::path> operands);

}
