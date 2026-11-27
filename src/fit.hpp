#pragma once

#include "media.hpp"
#include "sixel.hpp"
#include "terminal.hpp"

namespace si {

// Dimensions and sample aspect are those of the already-oriented frame.
Dimensions fit_dimensions(Dimensions source, AVRational sample_aspect,
    std::optional<Dimensions> viewport);

// The result borrows the decoded RGB or the caller's reusable scaled pixels.
// Keep the borrowed storage unchanged until the view has been encoded.
RgbView fit_frame(const DisplayFrame& frame, std::optional<Dimensions> viewport,
    std::vector<std::uint8_t>& scaled_rgb);

}
