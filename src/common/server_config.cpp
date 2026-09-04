// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/server_config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include "ckgit/text.hpp"

namespace ckgit {
namespace {

constexpr std::size_t kMaximumConfigBytes = 64 * 1024;
constexpr std::size_t kMaximumLineBytes = 2048;
constexpr std::size_t kMaximumPathBytes = 1024;

bool hasOuterWhitespace(std::string_view value) {
  return !value.empty() &&
         (std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
          std::isspace(static_cast<unsigned char>(value.back())) != 0);
}

bool isAbsoluteConfiguredPath(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumPathBytes && value.front() == '/' &&
         isValidUtf8(value) && !hasControlCharacter(value);
}

[[noreturn]] void configError(const std::filesystem::path& path, std::size_t line,
                              std::string_view message) {
  throw std::runtime_error("invalid server configuration " + path.string() + ":" +
                           std::to_string(line) + ": " + std::string(message));
}

}  // namespace

ServerConfig loadServerConfig(const std::filesystem::path& path) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    throw std::runtime_error("server configuration must be a regular non-symlink file: " +
                             path.string());
  }
  const auto size = std::filesystem::file_size(path, status_error);
  if (status_error || size > kMaximumConfigBytes) {
    throw std::runtime_error("server configuration is too large or unavailable: " + path.string());
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open server configuration: " + path.string());
  }

  ServerConfig config;
  std::unordered_set<std::string> seen_fields;
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
    if (equals == std::string::npos || equals == 0) {
      configError(path, line_number, "expected key=value");
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (hasControlCharacter(key) || hasControlCharacter(value) || hasOuterWhitespace(key) ||
        hasOuterWhitespace(value) || key.find('=') != std::string::npos) {
      configError(path, line_number, "keys and values may not contain control or outer whitespace");
    }
    if (!seen_fields.insert(key).second) {
      configError(path, line_number, "duplicate field");
    }
    if (key == "schema_version") {
      if (value != "1") {
        configError(path, line_number, "unsupported schema_version");
      }
      has_schema = true;
    } else if (key == "repo_root" || key == "control_socket" || key == "state_root" ||
               key == "hook_directory") {
      if (!isAbsoluteConfiguredPath(value) || value.find('=') != std::string::npos) {
        configError(path, line_number, "expected an absolute path without '='");
      }
      if (key == "repo_root") {
        config.repo_root = value;
      } else if (key == "control_socket") {
        config.control_socket = value;
      } else if (key == "state_root") {
        config.state_root.emplace(value);
      } else {
        config.hook_directory.emplace(value);
      }
    } else if (key == "http_port") {
      unsigned int port = 0;
      const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (value.empty() || value.size() > 5 || parse_error != std::errc{} ||
          end != value.data() + value.size() || port > 65535) {
        configError(path, line_number, "http_port must be 0 to 65535");
      }
      config.http_port = static_cast<unsigned short>(port);
    } else {
      configError(path, line_number, "unknown field");
    }
  }
  if (!file.eof()) {
    throw std::runtime_error("could not read server configuration: " + path.string());
  }
  if (!has_schema || config.repo_root.empty() || config.control_socket.empty()) {
    throw std::runtime_error("server configuration is missing schema_version, repo_root, or control_socket: " +
                             path.string());
  }
  return config;
}

std::string renderServerConfig(const ServerConfig& config) {
  std::string rendered = "schema_version=1\nrepo_root=" + config.repo_root.string() +
                         "\ncontrol_socket=" + config.control_socket.string() + "\n";
  if (config.state_root.has_value()) {
    rendered += "state_root=" + config.state_root->string() + "\n";
  }
  if (config.hook_directory.has_value()) {
    rendered += "hook_directory=" + config.hook_directory->string() + "\n";
  }
  if (config.http_port.has_value()) {
    rendered += "http_port=" + std::to_string(*config.http_port) + "\n";
  }
  return rendered;
}

}  // namespace ckgit
