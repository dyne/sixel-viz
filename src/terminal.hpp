#pragma once

#include <optional>
#include <string_view>

namespace si {

struct Dimensions {
  int width = 0;
  int height = 0;
};

struct TerminalMetrics {
  Dimensions cells;
  Dimensions pixels;
  Dimensions cell_pixels;
};

struct Viewport {
  Dimensions usable_pixels;
  bool approximate;
  Dimensions cells{};
  Dimensions pixels{};
};

inline constexpr Dimensions default_cell_size{8, 16};

std::optional<Dimensions> parse_cell_size(std::string_view value);
Viewport resolve_viewport(const TerminalMetrics& metrics,
    Dimensions fallback_cell_size = default_cell_size);
// Read the current terminal size attached to stdout. A redirected stream has no viewport.
std::optional<Viewport> measure_stdout(Dimensions fallback_cell_size = default_cell_size);

}
