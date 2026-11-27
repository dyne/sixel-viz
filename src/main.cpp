#include "fit.hpp"
#include "media.hpp"
#include "playback.hpp"
#include "sixel.hpp"
#include "terminal.hpp"
#include "terminal_playback.hpp"

#include <xtd/console>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

extern "C" {
#include <libavutil/log.h>
}

namespace {
constexpr int media_error = 1;
constexpr int usage_error = 2;
constexpr int interrupted_error = 130;
constexpr auto usage = "Usage: si [--] FILE\n       si --help";

#ifdef _WIN32
using Argument = std::wstring_view;
constexpr Argument help_option = L"--help";
constexpr Argument end_options = L"--";
#else
using Argument = std::string_view;
constexpr Argument help_option = "--help";
constexpr Argument end_options = "--";
#endif

std::string utf8_path(const std::filesystem::path& path) {
  const auto bytes = path.u8string();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void prepare_stdout() {
#ifdef _WIN32
  const int descriptor = _fileno(stdout);
  if (descriptor < 0 || _setmode(descriptor, _O_BINARY) == -1) {
    throw std::runtime_error("could not set stdout to binary mode");
  }
#else
  if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    throw std::runtime_error("could not configure stdout pipe handling");
  }
#endif
}

std::size_t write_stdout(std::span<const char> data) {
  while (true) {
    errno = 0;
    const auto written = std::fwrite(data.data(), 1, data.size(), stdout);
    if (!std::ferror(stdout)) return written;
    const int error = errno;
    if (error != EINTR) {
      throw std::system_error(error ? error : EIO, std::generic_category(), "write SIXEL output");
    }
    std::clearerr(stdout);
    if (written > 0) return written;
  }
}

int display_file(const std::filesystem::path& path, si::Dimensions cell_size) {
  const auto name = utf8_path(path);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    xtd::console::error << "si: cannot open '" << name << "' as a local file\n";
    return media_error;
  }

  // Report decoder failures with the local filename in the CLI diagnostic.
  av_log_set_level(AV_LOG_QUIET);
  try {
    const auto viewport = si::measure_stdout(cell_size);
    si::MediaDecoder decoder(path);
    auto first = decoder.next_frame();
    if (!first) {
      xtd::console::error << "si: '" << name << "' has no displayable frame\n";
      return media_error;
    }
    auto second = decoder.next_frame();
    if (second && viewport) {
      si::TerminalPlayback terminal{cell_size};
      const si::PlaybackClock clock{
        std::chrono::steady_clock::now,
        [&](auto until, const si::DisplayFrame& frame) { terminal.hold_until(until, frame); },
      };
      si::play_once(std::move(*first), std::move(*second), decoder.frame_rate(),
          [&] { terminal.check_interrupt(); return decoder.next_frame(); },
          [&](const si::DisplayFrame& frame) { terminal.present(frame); }, &clock);
      terminal.finish();
      return 0;
    }

    const auto bounds = viewport ? std::optional{viewport->usable_pixels} : std::nullopt;
    prepare_stdout();
    std::vector<std::uint8_t> scaled_rgb;
    const si::SixelSink sink{write_stdout, [] { return std::fflush(stdout) == 0; }};
    const auto present = [&](const si::DisplayFrame& frame) {
      si::encode_sixel(si::fit_frame(frame, bounds, scaled_rgb), sink);
    };
    if (second) {
      si::play_once(std::move(*first), std::move(*second), decoder.frame_rate(),
                    [&] { return decoder.next_frame(); }, present, nullptr);
    } else {
      present(*first);
    }
  } catch (const si::PlaybackInterrupted&) {
    return interrupted_error;
  } catch (const std::exception& failure) {
    xtd::console::error << "si: '" << name << "': " << failure.what() << '\n';
    return media_error;
  }

  return 0;
}

int run(int argc, const Argument::value_type* const* argv) {
  if (argc == 2 && Argument{argv[1]} == help_option) {
    xtd::console::write_line(usage);
    return 0;
  }

  const bool options_ended = argc > 1 && Argument{argv[1]} == end_options;
  const int operand_index = options_ended ? 2 : 1;
  if (argc == operand_index + 1) {
    const Argument operand{argv[operand_index]};
    if (!operand.empty() && (options_ended || !operand.starts_with('-'))) {
      const char* cell_setting = std::getenv("SI_CELL_SIZE");
      const auto cell_size = cell_setting ? si::parse_cell_size(cell_setting) :
                                           std::optional{si::default_cell_size};
      if (!cell_size) {
        xtd::console::error << "si: SI_CELL_SIZE must be WIDTHxHEIGHT in positive integer pixels\n"
                            << usage << '\n';
        return usage_error;
      }
      try {
        return display_file(std::filesystem::path{operand}, *cell_size);
      } catch (const std::filesystem::filesystem_error&) {
        xtd::console::error << "si: could not represent the input file path\n";
        return media_error;
      }
    }
  }

  xtd::console::error << usage << '\n';
  return usage_error;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  return run(argc, argv);
}
#else
int main(int argc, char** argv) {
  return run(argc, argv);
}
#endif
