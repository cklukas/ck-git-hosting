// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/validation.hpp"

#include <algorithm>
#include <cctype>

namespace ckgit {
namespace {

bool isAsciiProjectCharacter(unsigned char character) {
  return std::isalnum(character) != 0 || character == '.' || character == '_' ||
         character == '-';
}

bool isReservedName(std::string_view name) {
  std::string lower{name};
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower == "con" || lower == "prn" || lower == "aux" || lower == "nul" ||
         (lower.size() == 4 && lower.rfind("com", 0) == 0 &&
          lower[3] >= '1' && lower[3] <= '9') ||
         (lower.size() == 4 && lower.rfind("lpt", 0) == 0 &&
          lower[3] >= '1' && lower[3] <= '9');
}

}  // namespace

std::string projectNameError(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return "must contain 1 to 64 characters";
  }
  if (name.front() == '.') {
    return "must not begin with a dot";
  }
  if (name.find("..") != std::string_view::npos) {
    return "must not contain consecutive dots";
  }
  if (name.size() >= 4 && name.substr(name.size() - 4) == ".git") {
    return "must not end in .git";
  }
  if (isReservedName(name)) {
    return "is a reserved device name";
  }
  if (std::any_of(name.begin(), name.end(), [](unsigned char c) {
        return !isAsciiProjectCharacter(c);
      })) {
    return "may contain only ASCII letters, digits, dot, underscore, and hyphen";
  }
  return {};
}

bool isValidProjectName(std::string_view name) {
  return projectNameError(name).empty();
}

bool isValidClientId(std::string_view client_id) {
  if (client_id.empty() || client_id.size() > 64 || client_id.front() == '-') {
    return false;
  }
  return std::all_of(client_id.begin(), client_id.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_';
  });
}

namespace {

bool isLowercaseHex(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}

}  // namespace

bool isValidObjectId(std::string_view object_id) {
  return (object_id.size() == 40 || object_id.size() == 64) && isLowercaseHex(object_id);
}

bool isValidSha256Hex(std::string_view digest) {
  return digest.size() == 64 && isLowercaseHex(digest);
}

bool isValidBranchName(std::string_view branch) {
  if (branch.empty() || branch.size() > 255 || branch.front() == '-' || branch.front() == '/' ||
      branch.back() == '/' || branch.back() == '.' || branch.find("..") != std::string_view::npos ||
      branch.find("@{") != std::string_view::npos || branch.find("//") != std::string_view::npos) {
    return false;
  }
  for (const unsigned char character : branch) {
    if (character <= 0x20 || character == 0x7f || character == '\\' || character == '~' ||
        character == '^' || character == ':' || character == '?' || character == '*' ||
        character == '[') {
      return false;
    }
  }
  std::size_t start = 0;
  while (start < branch.size()) {
    const std::size_t slash = branch.find('/', start);
    const std::string_view component =
        branch.substr(start, slash == std::string_view::npos ? branch.size() - start : slash - start);
    if (component.empty() || component == "." ||
        (component.size() >= 5 && component.substr(component.size() - 5) == ".lock")) {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

bool isSafeRelativePath(std::string_view path) {
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path.empty() || path.size() > kMaximumSafeRelativePathBytes || path.front() == '/') return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view component =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (component.empty() || component == "." || component == "..") return false;
    for (const unsigned char character : component) {
      if (character < 0x20 || character == 0x7f || character == '\\') return false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

bool isValidCiLabel(std::string_view label) {
  if (label.empty() || label.size() > kMaximumCiLabelBytes) return false;
  const auto lower_or_digit = [](unsigned char byte) { return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9'); };
  if (!lower_or_digit(static_cast<unsigned char>(label.front()))) return false;
  return std::all_of(label.begin(), label.end(), [&](unsigned char byte) {
    return lower_or_digit(byte) || byte == '.' || byte == '_' || byte == '-';
  });
}

std::optional<std::vector<std::string>> parseCiLabelList(std::string_view text) {
  std::vector<std::string> labels;
  if (text.empty()) return labels;
  for (std::size_t start = 0;;) {
    const std::size_t comma = text.find(',', start);
    const std::string_view label = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (!isValidCiLabel(label)) return std::nullopt;
    labels.emplace_back(label);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  std::sort(labels.begin(), labels.end());
  labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
  if (labels.size() > kMaximumCiLabels) return std::nullopt;
  return labels;
}

std::string formatCiLabelList(const std::vector<std::string>& labels) {
  std::string text;
  for (const auto& label : labels) text += (text.empty() ? "" : ",") + label;
  return text;
}

bool ciRunnerMatches(std::string_view name, const std::vector<std::string>& labels,
                     const std::vector<std::string>& runs_on) {
  return std::all_of(runs_on.begin(), runs_on.end(), [&](const std::string& wanted) {
    return wanted == name || std::find(labels.begin(), labels.end(), wanted) != labels.end();
  });
}

}  // namespace ckgit
