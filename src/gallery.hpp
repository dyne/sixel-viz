#pragma once

#include "sixel.hpp"
#include "terminal.hpp"

#include <filesystem>
#include <span>

namespace si {

int display_gallery(std::span<const std::filesystem::path> paths,
                    Dimensions cell_size, const SixelSink& sink);

}
