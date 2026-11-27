#include <xtd/console>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

extern "C" {
#include <libavformat/avio.h>
}

namespace {
constexpr int media_error = 1;
constexpr int usage_error = 2;
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

int display_file(const std::filesystem::path& path) {
  const auto name = utf8_path(path);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    xtd::console::error << "si: cannot open '" << name << "' as a local file\n";
    return media_error;
  }

  // Select FFmpeg's file protocol explicitly and pass it the literal path.
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    xtd::console::error << "si: cannot resolve the path '" << name << "'\n";
    return media_error;
  }
  const auto url = "file:" + utf8_path(absolute);
  AVIOContext* input = nullptr;
  if (avio_open2(&input, url.c_str(), AVIO_FLAG_READ, nullptr, nullptr) < 0) {
    xtd::console::error << "si: cannot read '" << name << "'\n";
    return media_error;
  }
  avio_closep(&input);

  xtd::console::error << "si: opened '" << name << "', but display is unavailable\n";
  return media_error;
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
      try {
        return display_file(std::filesystem::path{operand});
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
