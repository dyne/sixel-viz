#include "terminal.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void check_viewport(si::Viewport actual, int width, int height, bool approximate) {
  require(actual.usable_pixels.width == width, "unexpected usable width");
  require(actual.usable_pixels.height == height, "unexpected usable height");
  require(actual.approximate == approximate, "unexpected viewport accuracy");
}

void check_cell_size() {
  const auto size = si::parse_cell_size("10x20");
  require(size.has_value(), "expected a parsed cell size");
  check_viewport(si::resolve_viewport({{80, 24}, {}, {}}, *size), 800, 460, true);
  for (const auto value : {"", "x", "8", "x16", "8x", "0x16", "8x0", "-1x16", "+8x16",
                          "8X16", "8x16x20", "8x16 ", " 8x16", "999999999999x16"}) {
    require(!si::parse_cell_size(value), "accepted an invalid cell size");
  }
  const int largest = std::numeric_limits<int>::max();
  check_viewport(si::resolve_viewport({}, {largest, largest}), largest, largest, true);
}

}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string_view test = argv[1];

  try {
    if (test == "pixel_metrics") {
      check_viewport(si::resolve_viewport({{80, 30}, {800, 600}, {8, 16}}, {3, 4}), 800, 580, false);
      check_viewport(si::resolve_viewport({{80, 30}, {800, 601}, {}}), 800, 580, false);
    } else if (test == "measured_cells") {
      check_viewport(si::resolve_viewport({{80, 24}, {800, 0}, {9, 19}}, {3, 4}), 720, 437, false);
    } else if (test == "cell_only") {
      check_viewport(si::resolve_viewport({{100, 30}, {}, {8, 0}}), 800, 464, true);
    } else if (test == "zero_size") {
      check_viewport(si::resolve_viewport({}), 640, 368, true);
      check_viewport(si::resolve_viewport({{0, 30}, {800, 0}, {}}), 640, 464, true);
    } else if (test == "one_row") {
      check_viewport(si::resolve_viewport({{80, 1}, {800, 20}, {}}), 800, 20, false);
      check_viewport(si::resolve_viewport({{80, 1}, {}, {}}), 640, 16, true);
    } else if (test == "cell_size") {
      check_cell_size();
    } else if (test == "pipe") {
      require(!si::measure_stdout(), "piped stdout has a terminal viewport");
    } else if (test == "stdout") {
      // The live terminal check captures this stream and leaves stdout on the TTY.
      const auto viewport = si::measure_stdout();
      require(viewport.has_value(), "expected stdout to be a terminal");
      std::cerr << viewport->usable_pixels.width << 'x' << viewport->usable_pixels.height
                << (viewport->approximate ? " approximate\n" : " measured\n");
    } else {
      return 2;
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
