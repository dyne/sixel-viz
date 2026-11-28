#include "gallery.hpp"

#include "fit.hpp"
#include "gallery_input.hpp"
#include "media.hpp"
#include "playback.hpp"
#include "terminal_playback.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

extern "C" {
#include <libavutil/avstring.h>
}

namespace si {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono;

struct Entry {
  fs::path path;
  sys_time<microseconds> time;
  std::string label;
};

struct Label {
  std::string text;
  std::size_t width = 0;
};

std::string utf8_path(const fs::path& path) {
  const auto bytes = path.generic_u8string();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

int character_width(int codepoint) {
  if (codepoint < 0x80) return 1;
#ifndef _WIN32
  if (const int width = wcwidth(static_cast<wchar_t>(codepoint)); width >= 0) return width;
#endif
  return 2;
}

Label printable(std::string_view text, std::size_t columns = std::string_view::npos) {
  Label result;
  const auto* next = reinterpret_cast<const std::uint8_t*>(text.data());
  const auto* end = next + text.size();
  std::size_t previous_size = 0;
  while (next != end) {
    const auto* start = next;
    std::int32_t codepoint = -1;
    const auto status = av_utf8_decode(&codepoint, &next, end, 0);
    if (next == start) ++next;
    const bool unsafe = status < 0 || codepoint < 32 || (codepoint >= 127 && codepoint < 160);
    const int width = unsafe ? 1 : character_width(codepoint);
    if (result.width + width > columns) {
      if (result.width == columns) result.text.resize(previous_size);
      if (columns > 0) result.text += '~';
      break;
    }
    if (width > 0) previous_size = result.text.size();
    if (unsafe) result.text += '?';
    else result.text.append(reinterpret_cast<const char*>(start), next - start);
    result.width += width;
  }
  return result;
}

std::string relative_name(const fs::path& path, const fs::path& working_directory) {
  const auto absolute = (working_directory / path).lexically_normal();
  const auto relative = absolute.lexically_relative(working_directory);
  return utf8_path(relative.empty() ? absolute : relative);
}

std::string human_size(std::uintmax_t bytes) {
  constexpr std::string_view units = "BKMG";
  std::size_t suffix = 0;
  std::uintmax_t unit = 1;
  while (suffix + 1 < units.size() && bytes / unit >= 1024) {
    unit *= 1024;
    ++suffix;
  }
  auto whole = bytes / unit;
  auto decimal = ((bytes % unit) * 10 + unit / 2) / unit;
  if (decimal == 10) {
    ++whole;
    decimal = 0;
    if (whole == 1024 && suffix + 1 < units.size()) {
      whole = 1;
      ++suffix;
    }
  }
  auto text = std::to_string(whole);
  if (decimal != 0) text += '.' + std::to_string(decimal);
  return text + units[suffix];
}

year_month_day local_date(sys_time<microseconds> time) {
  const auto seconds_since_epoch = system_clock::to_time_t(floor<seconds>(time));
  std::tm local{};
#ifdef _WIN32
  const bool have_local_date = localtime_s(&local, &seconds_since_epoch) == 0;
#else
  const bool have_local_date = localtime_r(&seconds_since_epoch, &local) != nullptr;
#endif
  if (!have_local_date) return year_month_day{floor<days>(time)};
  return {year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)},
          day{static_cast<unsigned>(local.tm_mday)}};
}

Entry describe(const fs::path& path, const fs::path& working_directory, const MediaDecoder& decoder) {
  const auto capture = decoder.capture_time();
  const auto time = capture ? sys_time<microseconds>{capture->instant} :
      floor<microseconds>(file_clock::to_sys(fs::last_write_time(path)));
  const auto date = capture ? capture->date : local_date(time);
  const auto two_digits = [](unsigned n) {
    return std::string{static_cast<char>('0' + n / 10), static_cast<char>('0' + n % 10)};
  };
  const auto year = static_cast<unsigned>((int(date.year()) % 100 + 100) % 100);
  const auto formatted = two_digits(unsigned(date.day())) + '/' +
      two_digits(unsigned(date.month())) + '/' + two_digits(year);
  return {path, time, formatted + ' ' + human_size(fs::file_size(path)) + ' ' +
          relative_name(path, working_directory)};
}

void report_error(const fs::path& path, std::string_view error) {
  const auto name = printable(utf8_path(path)).text;
  if (error == "cannot open as a local file") {
    std::cerr << "si: cannot open '" << name << "' as a local file\n";
  } else {
    std::cerr << "si: '" << name << "': " << error << '\n';
  }
}

DisplayFrame first_frame(MediaDecoder& decoder) {
  auto frame = decoder.next_frame();
  if (!frame) throw std::runtime_error("has no displayable frame");
  return std::move(*frame);
}

void write_text(const SixelSink& sink, std::string_view text) {
  while (!text.empty()) {
    const auto written = sink.write(text);
    if (written == 0 || written > text.size()) throw std::runtime_error("could not write preview text");
    text.remove_prefix(written);
  }
}

class ConsolePreviewMode {
public:
  explicit ConsolePreviewMode(bool interactive) {
#ifdef _WIN32
    if (!interactive) return;
    output_ = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
    if (!GetConsoleMode(output_, &mode_) ||
        !SetConsoleMode(output_, mode_ | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      throw std::runtime_error("could not enable console preview output");
    }
    code_page_ = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#else
    (void)interactive;
#endif
  }

  ~ConsolePreviewMode() {
#ifdef _WIN32
    if (output_ != INVALID_HANDLE_VALUE) {
      if (code_page_ != 0) SetConsoleOutputCP(code_page_);
      SetConsoleMode(output_, mode_);
    }
#endif
  }

  ConsolePreviewMode(const ConsolePreviewMode&) = delete;
  ConsolePreviewMode& operator=(const ConsolePreviewMode&) = delete;

private:
#ifdef _WIN32
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD mode_ = 0;
  UINT code_page_ = 0;
#endif
};

struct Layout {
  std::optional<Viewport> terminal;
  Viewport geometry;
  int right_column;

  Layout(std::optional<Viewport> viewport, Dimensions cell_size, bool batch)
      : terminal(viewport), geometry(viewport.value_or(resolve_viewport({}, cell_size))),
        right_column(batch && geometry.cells.width >= 3 ? geometry.cells.width / 2 : 0) {}

  int column_pixels(int column) const {
    return static_cast<int>(static_cast<std::int64_t>(column) * geometry.pixels.width /
                            geometry.cells.width);
  }

  std::optional<Dimensions> bounds(bool right) const {
    if (!terminal && right_column == 0) return std::nullopt;
    const int width = right_column == 0 ? geometry.pixels.width :
        right ? geometry.pixels.width - column_pixels(right_column) :
                column_pixels(std::max(1, right_column - 1));
    const auto caption_pixels = terminal ?
        (2LL * geometry.pixels.height + geometry.cells.height - 1) / geometry.cells.height : 0LL;
    const auto height = std::max(1LL, geometry.usable_pixels.height - caption_pixels);
    return Dimensions{std::max(1, width), static_cast<int>(height)};
  }

  bool position_raster() const { return terminal && geometry.cells.height >= 3; }

  std::string_view line_ending() const { return terminal ? "\r\n" : "\n"; }

  int caption_columns(bool right) const {
    if (right) return geometry.cells.width - right_column;
    return right_column ? right_column - 1 : geometry.cells.width;
  }

  int image_rows(std::size_t height) const {
    return static_cast<int>((static_cast<std::int64_t>(height) * geometry.cells.height +
                             geometry.pixels.height - 1) / geometry.pixels.height);
  }
};

struct Thumbnail {
  std::vector<std::uint8_t> rgb;
  std::size_t width;
  std::size_t height;

  RgbView image() const { return {rgb, width, height, width * 3}; }
};

void overlay_play(Thumbnail& thumbnail) {
  const auto minimum = std::min(thumbnail.width, thumbnail.height);
  const auto size = static_cast<int>(std::min<std::size_t>(80, std::max<std::size_t>(1, minimum * 3 / 4)));
  const int margin = size / 5;
  const int triangle = size - 2 * margin;
  const auto left = (thumbnail.width - size) / 2;
  const auto top = (thumbnail.height - size) / 2;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const int dx = x - margin;
      const bool inside = dx >= 0 && dx < triangle &&
          std::abs(2 * y - size + 1) <= triangle - dx - 1;
      const auto pixel = ((top + y) * thumbnail.width + left + x) * 3;
      for (int channel = 0; channel < 3; ++channel) {
        auto& component = thumbnail.rgb[pixel + channel];
        component = inside ? 255 : component / 3;
      }
    }
  }
}

Thumbnail thumbnail(DisplayFrame frame, std::optional<Dimensions> bounds, bool video) {
  std::vector<std::uint8_t> scaled;
  const auto fitted = fit_frame(frame, bounds, scaled);
  auto rgb = scaled.empty() ? std::move(frame.rgb) : std::move(scaled);
  Thumbnail result{std::move(rgb), fitted.width, fitted.height};
  if (video) overlay_play(result);
  return result;
}

Thumbnail load_thumbnail(const fs::path& path, std::optional<Dimensions> bounds) {
  MediaDecoder decoder(path);
  auto frame = first_frame(decoder);
  const bool video = decoder.is_video();
  if (video) frame = decoder.video_preview(std::move(frame));
  return thumbnail(std::move(frame), bounds, video);
}

Thumbnail compose_row(const Thumbnail& left, const Thumbnail& right, std::size_t right_origin) {
  const auto width = right_origin + right.width;
  const auto height = std::max(left.height, right.height);
  Thumbnail row{std::vector<std::uint8_t>(sixel_rgb_size(width, height)), width, height};
  const auto copy = [&](const Thumbnail& image, std::size_t x) {
    // A composed raster shares one caption line. Align both bottom edges.
    const auto top = height - image.height;
    const auto stride = image.width * 3;
    for (std::size_t y = 0; y < image.height; ++y) {
      std::copy_n(image.rgb.data() + y * stride, stride, row.rgb.data() + ((top + y) * width + x) * 3);
    }
  };
  copy(left, 0);
  copy(right, right_origin);
  return row;
}

constexpr std::string_view restore_cursor = "\x1b" "8";

std::string cursor_up(int rows) {
  return rows > 0 ? "\x1b[" + std::to_string(rows) + "A" : std::string{};
}

std::string cursor_column(int column) {
  return "\x1b[" + std::to_string(column + 1) + "G";
}

void captions(const SixelSink& sink, const Layout& layout, const Entry& left, const Entry* right) {
  const auto left_label = printable(left.label,
      layout.terminal ? layout.caption_columns(false) : std::string_view::npos);
  write_text(sink, left_label.text);
  if (right) {
    if (layout.terminal) {
      write_text(sink, cursor_column(layout.right_column));
    } else {
      const auto column = static_cast<std::size_t>(layout.right_column);
      write_text(sink, std::string(column > left_label.width ? column - left_label.width : 1, ' '));
    }
    write_text(sink, printable(right->label,
        layout.terminal ? layout.caption_columns(true) : std::string_view::npos).text);
  }
  write_text(sink, layout.line_ending());
}

void end_row(const SixelSink& sink, const Layout& layout) {
  write_text(sink, layout.line_ending());
  if (!sink.flush()) throw std::runtime_error("could not flush preview text");
}

struct Tile {
  const Thumbnail& thumbnail;
  const Entry& entry;
};

class GalleryColumns {
public:
  bool use(const Layout& layout) {
    const auto [width, height] = layout.geometry.cells;
    const auto [pixel_width, pixel_height] = layout.geometry.pixels;
    const bool positioned = layout.position_raster() && layout.right_column > 0;
    if (!positioned || width != cells_.width || height != cells_.height ||
        pixel_width != pixels_.width || pixel_height != pixels_.height) {
      // Start a new group if the terminal reflows or the column widths change.
      left_space_ = right_space_ = 0;
      cells_ = {width, height};
      pixels_ = {pixel_width, pixel_height};
    }
    return positioned;
  }

  bool next_is_right() const { return right_space_ > left_space_; }

  void present(const SixelSink& sink, const Layout& layout, Tile tile, bool right) {
    const ConsolePreviewMode mode(true);
    const int image_rows = layout.image_rows(tile.thumbnail.height);
    const int tile_rows = image_rows + 2;  // Caption and a blank line.
    auto& available = right ? right_space_ : left_space_;
    // Keep the saved cursor below both columns. Extend the terminal only when
    // the next tile needs more rows than are available above that cursor.
    const int advance = std::max(0, tile_rows - available);
    left_space_ += advance;
    right_space_ += advance;
    write_text(sink, "\r" + std::string(advance, '\n') + "\x1b" "7");
    const auto column = right ? cursor_column(layout.right_column) : std::string{};
    write_text(sink, cursor_up(available) + column);
    encode_sixel(tile.thumbnail.image(), sink);
    write_text(sink, restore_cursor);
    write_text(sink, cursor_up(available - image_rows) + column);
    write_text(sink, printable(tile.entry.label, layout.caption_columns(right)).text);
    write_text(sink, restore_cursor);
    if (!sink.flush()) throw std::runtime_error("could not flush preview text");
    available -= tile_rows;
  }

private:
  Dimensions cells_;
  Dimensions pixels_;
  int left_space_ = 0;
  int right_space_ = 0;
};

void present(const SixelSink& sink, const Layout& layout, Tile left, std::optional<Tile> right = {}) {
  const ConsolePreviewMode mode(layout.terminal.has_value());
  if (layout.position_raster()) {
    // Reserve the raster rows before drawing, then restore the cursor just
    // below them regardless of where the terminal leaves it after SIXEL.
    const auto rows = layout.image_rows(left.thumbnail.height);
    write_text(sink, "\r" + std::string(rows, '\n') + "\x1b" "7");
    write_text(sink, cursor_up(rows));
    encode_sixel(left.thumbnail.image(), sink);
    write_text(sink, restore_cursor);
  } else {
    if (right) {
      const auto row = compose_row(left.thumbnail, right->thumbnail, layout.column_pixels(layout.right_column));
      encode_sixel(row.image(), sink);
    } else {
      encode_sixel(left.thumbnail.image(), sink);
    }
    write_text(sink, layout.line_ending());
  }
  captions(sink, layout, left.entry, right ? &right->entry : nullptr);
  end_row(sink, layout);
}

void play_motion(MediaDecoder& decoder, DisplayFrame first, DisplayFrame second,
                 Dimensions cell_size, bool interactive, const SixelSink& sink) {
  if (interactive) {
    TerminalPlayback terminal{cell_size};
    const PlaybackClock clock{
      steady_clock::now,
      [&](auto until, const DisplayFrame& frame) { terminal.hold_until(until, frame); },
    };
    play_once(std::move(first), std::move(second), decoder.frame_rate(),
        [&] { terminal.check_interrupt(); return decoder.next_frame(); },
        [&](const DisplayFrame& frame) { terminal.present(frame); }, &clock);
    terminal.finish();
  } else {
    std::vector<std::uint8_t> scaled;
    play_once(std::move(first), std::move(second), decoder.frame_rate(),
        [&] { return decoder.next_frame(); },
        [&](const DisplayFrame& frame) { encode_sixel(fit_frame(frame, std::nullopt, scaled), sink); },
        nullptr);
  }
}

void display_single(const fs::path& path, const fs::path& cwd, Dimensions cell_size, const SixelSink& sink) {
  const auto viewport = measure_stdout(cell_size);
  const Layout layout(viewport, cell_size, false);
  MediaDecoder decoder(path);
  auto first = first_frame(decoder);
  const bool video = decoder.is_video();
  if (video) {
    if (auto second = decoder.next_frame()) {
      play_motion(decoder, std::move(first), std::move(*second), cell_size, viewport.has_value(), sink);
      return;
    }
  }

  const auto entry = describe(path, cwd, decoder);
  if (!video) {
    if (auto second = decoder.next_frame()) {
      // Keep a still on the primary screen when an animated image leaves
      // the alternate screen. Redirected motion already printed its frames.
      std::optional<DisplayFrame> still;
      if (viewport) still = first;
      play_motion(decoder, std::move(first), std::move(*second), cell_size, viewport.has_value(), sink);
      if (still) {
        const Layout resized(measure_stdout(cell_size), cell_size, false);
        const auto preview = thumbnail(std::move(*still), resized.bounds(false), false);
        present(sink, resized, {preview, entry});
      } else {
        write_text(sink, "\n");
        captions(sink, layout, entry, nullptr);
        end_row(sink, layout);
      }
      return;
    }
  }
  present(sink, layout, {thumbnail(std::move(first), layout.bounds(false), false), entry});
}

}

int display_gallery(std::span<const fs::path> paths, Dimensions cell_size, const SixelSink& sink) {
  std::setlocale(LC_CTYPE, "");
  const auto cwd = fs::current_path();
  const auto inputs = collect_gallery_inputs(paths);
  for (const auto& error : inputs.errors) report_error(error.path, error.message);
  if (!inputs.batch && !inputs.files.empty()) {
    const auto& path = inputs.files.front().path;
    try {
      display_single(path, cwd, cell_size, sink);
      return 0;
    } catch (const PlaybackInterrupted&) {
      throw;
    } catch (const std::exception& error) {
      report_error(path, error.what());
      return 1;
    }
  }

  bool failed = !inputs.errors.empty();
  std::vector<Entry> entries;
  for (const auto& file : inputs.files) {
    try {
      MediaDecoder decoder(file.path);
      first_frame(decoder);
      entries.push_back(describe(file.path, cwd, decoder));
    } catch (const std::exception& error) {
      if (file.explicit_file) {
        report_error(file.path, error.what());
        failed = true;
      }
    }
  }
  if (entries.empty()) {
    if (!failed) std::cerr << "si: no displayable media found\n";
    return 1;
  }
  std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
    if (left.time != right.time) return left.time < right.time;
    return left.path < right.path;
  });

  GalleryColumns columns;
  for (std::size_t index = 0; index < entries.size();) {
    const Layout layout(measure_stdout(cell_size), cell_size, true);
    const auto load = [&](bool right) -> std::optional<Thumbnail> {
      try {
        return load_thumbnail(entries[index].path, layout.bounds(right));
      } catch (const std::exception& error) {
        report_error(entries[index].path, error.what());
        failed = true;
        return std::nullopt;
      }
    };
    if (columns.use(layout)) {
      const bool right = columns.next_is_right();
      const auto preview = load(right);
      const auto& entry = entries[index++];
      if (preview) columns.present(sink, layout, {*preview, entry}, right);
      continue;
    }
    const auto left = load(false);
    const auto& left_entry = entries[index++];
    if (!left) continue;
    if (layout.right_column && index < entries.size()) {
      const auto right = load(true);
      const auto& right_entry = entries[index++];
      if (right) {
        present(sink, layout, {*left, left_entry}, Tile{*right, right_entry});
        continue;
      }
    }
    present(sink, layout, {*left, left_entry});
  }
  return failed ? 1 : 0;
}

}
