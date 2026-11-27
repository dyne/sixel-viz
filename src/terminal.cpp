#include "terminal.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace si {
namespace {

bool positive(Dimensions size) {
  return size.width > 0 && size.height > 0;
}

int parse_dimension(std::string_view text) {
  if (text.empty()) return 0;
  int size = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
  return error == std::errc{} && end == text.data() + text.size() && size > 0 ? size : 0;
}

int cell_extent(int count, int cell_size) {
  return static_cast<int>(std::min<std::int64_t>(
      static_cast<std::int64_t>(count) * cell_size, std::numeric_limits<int>::max()));
}

#ifdef _WIN32
TerminalMetrics console_metrics(HANDLE output) {
  TerminalMetrics metrics;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(output, &info)) {
    metrics.cells = {info.srWindow.Right - info.srWindow.Left + 1,
                     info.srWindow.Bottom - info.srWindow.Top + 1};
  }

  // A pseudoconsole's HWND has no displayed client area or visible font.
  const HWND window = GetConsoleWindow();
  if (!window || !IsWindowVisible(window) || IsIconic(window)) return metrics;

  CONSOLE_FONT_INFO font{};
  if (GetCurrentConsoleFont(output, FALSE, &font)) {
    const COORD size = GetConsoleFontSize(output, font.nFont);
    metrics.cell_pixels = {size.X, size.Y};
  }
  RECT area{};
  if (positive(metrics.cells) && positive(metrics.cell_pixels) && GetClientRect(window, &area)) {
    // The client area may include padding around the grid. Trust it as the
    // output viewport when it agrees with the visible cells and their font.
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width == cell_extent(metrics.cells.width, metrics.cell_pixels.width) &&
        height == cell_extent(metrics.cells.height, metrics.cell_pixels.height)) {
      metrics.pixels = {width, height};
    }
  }
  return metrics;
}
#endif

}

std::optional<Dimensions> parse_cell_size(std::string_view value) {
  const auto separator = value.find('x');
  if (separator == std::string_view::npos) return std::nullopt;
  const Dimensions size{parse_dimension(value.substr(0, separator)),
                        parse_dimension(value.substr(separator + 1))};
  if (!positive(size)) return std::nullopt;
  return size;
}

Viewport resolve_viewport(const TerminalMetrics& metrics, Dimensions fallback_cell_size) {
  if (positive(metrics.pixels)) {
    const int rows = metrics.cells.height;
    const int height = metrics.pixels.height;
    // Round the reserved row up so the raster stays within the usable area.
    const int prompt = rows > 1 ? height / rows + (height % rows != 0) : 0;
    return {{metrics.pixels.width, std::max(1, height - prompt)}, false};
  }

  const bool measured_cell_size = positive(metrics.cell_pixels);
  const Dimensions cell_size = measured_cell_size ? metrics.cell_pixels : fallback_cell_size;
  const int columns = metrics.cells.width > 0 ? metrics.cells.width : 80;
  const int rows = metrics.cells.height > 0 ? metrics.cells.height : 24;
  return {{cell_extent(columns, cell_size.width), cell_extent(std::max(1, rows - 1), cell_size.height)},
          !measured_cell_size || !positive(metrics.cells)};
}

std::optional<Viewport> measure_stdout(Dimensions fallback_cell_size) {
#ifdef _WIN32
  const int descriptor = _fileno(stdout);
  if (descriptor < 0) return std::nullopt;
  const auto output = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
  DWORD mode = 0;
  if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode)) return std::nullopt;
  return resolve_viewport(console_metrics(output), fallback_cell_size);
#else
  if (!isatty(STDOUT_FILENO)) return std::nullopt;
  struct winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1) {
    return resolve_viewport({}, fallback_cell_size);
  }
  return resolve_viewport({{size.ws_col, size.ws_row}, {size.ws_xpixel, size.ws_ypixel}, {}},
                          fallback_cell_size);
#endif
}

}
