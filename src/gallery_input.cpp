#include "gallery_input.hpp"

#include <map>
#include <string_view>
#include <utility>

namespace si {
namespace {

namespace fs = std::filesystem;

bool has_pattern(std::u32string_view text) {
  const auto bracket = text.find(U'[');
  return text.find_first_of(U"*?") != text.npos ||
      (bracket != text.npos && text.find(U']', bracket + 2) != text.npos);
}

char32_t matching_case(char32_t ch) {
#ifdef _WIN32
  if (ch >= U'A' && ch <= U'Z') return ch + (U'a' - U'A');
#endif
  return ch;
}

// Return how much of the pattern a single character consumed.
std::size_t match_character(std::u32string_view pattern, char32_t ch) {
  if (pattern.empty()) return 0;
  if (pattern.front() == U'?') return 1;
  ch = matching_case(ch);
  if (pattern.front() == U'[') {
    const auto end = pattern.find(U']', 2);
    if (end != pattern.npos) {
      const bool inverted = pattern[1] == U'!' || pattern[1] == U'^';
      bool found = false;
      for (std::size_t i = 1 + inverted; i < end; ++i) {
        const auto first = matching_case(pattern[i]);
        if (i + 2 < end && pattern[i + 1] == U'-') {
          const auto last = matching_case(pattern[i + 2]);
          found |= first <= ch && ch <= last;
          i += 2;
        } else {
          found |= first == ch;
        }
      }
      return found != inverted ? end + 1 : 0;
    }
  }
  return matching_case(pattern.front()) == ch ? 1 : 0;
}

bool matches(std::u32string_view pattern, std::u32string_view name) {
  if (name.starts_with(U'.') && !pattern.starts_with(U'.')) return false;
  std::size_t p = 0, n = 0;
  std::size_t after_star = pattern.npos, star_end = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == U'*') {
      after_star = ++p;
      star_end = n;
    } else if (const auto consumed = match_character(pattern.substr(p), name[n])) {
      p += consumed;
      ++n;
    } else if (after_star != pattern.npos) {
      // Retry the suffix with the most recent star consuming one more character.
      p = after_star;
      n = ++star_end;
    } else {
      return false;
    }
  }
  return pattern.substr(p).find_first_not_of(U'*') == pattern.npos;
}

std::vector<fs::path> expand_pattern(const fs::path& path) {
  std::vector<fs::path> matches_so_far{path.root_path()};
  for (const auto& component : path.relative_path()) {
    const auto pattern = component.u32string();
    if (!has_pattern(pattern)) {
      for (auto& prefix : matches_so_far) prefix /= component;
      continue;
    }

    std::vector<fs::path> next;
    for (const auto& prefix : matches_so_far) {
      const auto parent = prefix.empty() ? fs::path{"."} : prefix;
      if (!fs::is_directory(parent)) continue;
      for (const auto& entry : fs::directory_iterator(parent)) {
        const auto filename = entry.path().filename();
        if (matches(pattern, filename.u32string())) next.push_back(prefix / filename);
      }
    }
    matches_so_far = std::move(next);
  }
  return matches_so_far;
}

}

GalleryInputs collect_gallery_inputs(std::span<const fs::path> operands) {
  GalleryInputs result{{}, {}, operands.size() > 1};
  std::map<fs::path, bool> files;
  const auto add_discovered = [&](const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return false;
    files.try_emplace(path, false);
    return true;
  };

  for (const auto& operand : operands) {
    try {
      std::error_code status_error;
      const auto status = fs::status(operand, status_error);
      if (fs::is_regular_file(status)) {
        files[operand] = true;
      } else if (fs::is_directory(status)) {
        result.batch = true;
        for (const auto& entry : fs::directory_iterator(operand)) {
          add_discovered(operand / entry.path().filename());
        }
      } else if (!fs::exists(status) && has_pattern(operand.u32string())) {
        result.batch = true;
        bool found = false;
        for (const auto& match : expand_pattern(operand)) found |= add_discovered(match);
        if (!found) result.errors.push_back({operand, "no files match the pattern"});
      } else {
        result.errors.push_back({operand, "cannot open as a local file"});
      }
    } catch (const fs::filesystem_error& error) {
      result.errors.push_back({operand, error.code().message()});
    }
  }

  result.files.reserve(files.size());
  for (const auto& [path, explicit_file] : files) result.files.push_back({path, explicit_file});
  return result;
}

}
