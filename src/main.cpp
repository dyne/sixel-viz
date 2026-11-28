#include "gallery.hpp"
#include "terminal_playback.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
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
constexpr auto usage = "Usage: si [--] INPUT...\n"
                       "       si --help\n"
                       "INPUT can be a file, folder, or wildcard pattern.";

#ifdef _WIN32
using Argument = std::wstring_view;
constexpr Argument help_option = L"--help";
constexpr Argument end_options = L"--";
#else
using Argument = std::string_view;
constexpr Argument help_option = "--help";
constexpr Argument end_options = "--";
#endif

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

int run(int argc, const Argument::value_type* const* argv) {
  if (argc == 2 && Argument{argv[1]} == help_option) {
    std::cout << usage << '\n';
    return 0;
  }

  const bool options_ended = argc > 1 && Argument{argv[1]} == end_options;
  const int operand_index = options_ended ? 2 : 1;
  if (argc <= operand_index) {
    std::cerr << usage << '\n';
    return usage_error;
  }
  for (int i = operand_index; i < argc; ++i) {
    const Argument operand{argv[i]};
    if (operand.empty() || (!options_ended && operand.starts_with('-'))) {
      std::cerr << usage << '\n';
      return usage_error;
    }
  }

  const char* cell_setting = std::getenv("SI_CELL_SIZE");
  const auto cell_size = cell_setting ? si::parse_cell_size(cell_setting) :
                                       std::optional{si::default_cell_size};
  if (!cell_size) {
    std::cerr << "si: SI_CELL_SIZE must be WIDTHxHEIGHT in positive integer pixels\n"
              << usage << '\n';
    return usage_error;
  }
  try {
    std::vector<std::filesystem::path> paths;
    paths.reserve(argc - operand_index);
    for (int i = operand_index; i < argc; ++i) paths.emplace_back(Argument{argv[i]});
    av_log_set_level(AV_LOG_QUIET);
    prepare_stdout();
    const si::SixelSink sink{write_stdout, [] { return std::fflush(stdout) == 0; }};
    return si::display_gallery(paths, *cell_size, sink);
  } catch (const si::PlaybackInterrupted&) {
    return interrupted_error;
  } catch (const std::exception& error) {
    std::cerr << "si: " << error.what() << '\n';
    return media_error;
  }
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
