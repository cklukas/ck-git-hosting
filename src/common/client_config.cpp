// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/client_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

constexpr std::size_t kMaximumConfigBytes = 64 * 1024;
constexpr std::size_t kMaximumLineBytes = 2048;
constexpr std::size_t kMaximumListEntries = 64;

bool hasOuterWhitespace(std::string_view value) {
  return !value.empty() &&
         (std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
          std::isspace(static_cast<unsigned char>(value.back())) != 0);
}

bool isSafeDisplayName(std::string_view value) {
  return !value.empty() && value.size() <= 128 && !hasControlCharacter(value) && isValidUtf8(value);
}

bool isValidServer(std::string_view value) {
  const auto at = value.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 == value.size() ||
      value.find('@', at + 1) != std::string_view::npos || value.size() > 255 ||
      hasControlCharacter(value) || value.find_first_of("/:\\?#") != std::string_view::npos) {
    return false;
  }
  const std::string_view user = value.substr(0, at);
  const std::string_view host = value.substr(at + 1);
  return std::all_of(user.begin(), user.end(), [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '-' || character == '_';
         }) &&
         std::all_of(host.begin(), host.end(), [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '-' || character == '.';
         }) &&
         host.front() != '.' && host.back() != '.';
}

bool isValidRemoteName(std::string_view value) {
  return !value.empty() && value.size() <= 64 && value.front() != '-' && !hasControlCharacter(value) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                  character == '.';
         });
}

[[noreturn]] void configError(const std::filesystem::path& path, std::size_t line,
                              std::string_view message) {
  throw std::runtime_error("invalid client configuration " + path.string() + ":" +
                           std::to_string(line) + ": " + std::string(message));
}

}  // namespace

ClientConfig loadClientConfig(const std::filesystem::path& path) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    throw std::runtime_error("client configuration must be a regular non-symlink file: " +
                             path.string());
  }
  const auto size = std::filesystem::file_size(path, status_error);
  if (status_error || size > kMaximumConfigBytes) {
    throw std::runtime_error("client configuration is too large or unavailable: " + path.string());
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open client configuration: " + path.string());
  }

  ClientConfig config;
  std::unordered_set<std::string> singleton_fields;
  bool has_schema = false;
  std::size_t line_number = 0;
  for (std::string line; std::getline(file, line);) {
    ++line_number;
    if (line.size() > kMaximumLineBytes || (!line.empty() && line.back() == '\r')) {
      configError(path, line_number, "line is too long or uses CRLF");
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos || equals == 0 || line.find('=', equals + 1) != std::string::npos) {
      configError(path, line_number, "expected exactly one key=value separator");
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (hasControlCharacter(key) || hasControlCharacter(value) || hasOuterWhitespace(key) || hasOuterWhitespace(value)) {
      configError(path, line_number, "keys and values may not contain control or outer whitespace");
    }
    const bool repeated = key == "scan_root" || key == "exclude";
    if (!repeated && !singleton_fields.insert(key).second) {
      configError(path, line_number, "duplicate singleton field");
    }
    if (key == "schema_version") {
      if (value != "1") {
        configError(path, line_number, "unsupported schema_version");
      }
      has_schema = true;
    } else if (key == "client_id") {
      if (!isValidClientId(value)) {
        configError(path, line_number, "invalid client_id");
      }
      config.client_id = value;
    } else if (key == "display_name") {
      if (!isSafeDisplayName(value)) {
        configError(path, line_number, "invalid display_name");
      }
      config.display_name = value;
    } else if (key == "server") {
      if (!isValidServer(value)) {
        configError(path, line_number, "invalid server; expected user@host");
      }
      config.server = value;
    } else if (key == "remote_name") {
      if (!isValidRemoteName(value)) {
        configError(path, line_number, "invalid remote_name");
      }
      config.remote_name = value;
    } else if (key == "scan_root") {
      if (value.empty() || value.front() != '/' || !isValidUtf8(value) ||
          config.scan_roots.size() == kMaximumListEntries) {
        configError(path, line_number, "invalid or excessive scan_root");
      }
      config.scan_roots.emplace_back(value);
    } else if (key == "exclude") {
      if (value.empty() || value.size() > 256 || !isValidUtf8(value) ||
          config.exclusions.size() == kMaximumListEntries) {
        configError(path, line_number, "invalid or excessive exclude pattern");
      }
      config.exclusions.push_back(value);
    } else if (key == "public_path_mode") {
      if (value == "basename") {
        config.expose_full_paths = false;
      } else if (value == "full") {
        config.expose_full_paths = true;
      } else {
        configError(path, line_number, "public_path_mode must be basename or full");
      }
    } else {
      configError(path, line_number, "unknown field");
    }
  }
  if (!file.eof()) {
    throw std::runtime_error("could not read client configuration: " + path.string());
  }
  if (!has_schema || config.client_id.empty() || config.display_name.empty() || config.server.empty() ||
      config.remote_name.empty()) {
    throw std::runtime_error("client configuration is missing a required field: " + path.string());
  }
  const auto absolute = std::filesystem::absolute(path, status_error);
  if (status_error || !absolute.has_parent_path()) {
    throw std::runtime_error("cannot resolve client configuration directory: " + path.string());
  }
  config.config_directory = absolute.parent_path();
  return config;
}

}  // namespace ckgit
