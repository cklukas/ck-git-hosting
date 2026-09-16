// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/server_config.hpp"

#include <algorithm>
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

bool isValidSshCloneTarget(std::string_view value) {
  const auto at = value.find('@');
  if (value.empty() || value.size() > 255 || at == std::string_view::npos || at == 0 ||
      at + 1 == value.size() || value.find('@', at + 1) != std::string_view::npos) return false;
  const auto safe = [](std::string_view component) {
    return component.front() != '-' && component.front() != '.' && component.back() != '.' &&
        std::all_of(component.begin(), component.end(), [](unsigned char byte) {
          return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                 (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
        });
  };
  return safe(value.substr(0, at)) && safe(value.substr(at + 1));
}

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
               key == "hook_directory" || key == "ci_build_root" || key == "pages_root") {
      if (!isAbsoluteConfiguredPath(value) || value.find('=') != std::string::npos) {
        configError(path, line_number, "expected an absolute path without '='");
      }
      if (key == "repo_root") {
        config.repo_root = value;
      } else if (key == "control_socket") {
        config.control_socket = value;
      } else if (key == "state_root") {
        config.state_root.emplace(value);
      } else if (key == "ci_build_root") {
        config.ci_build_root.emplace(value);
      } else if (key == "pages_root") {
        config.pages_root.emplace(value);
      } else {
        config.hook_directory.emplace(value);
      }
    } else if (key == "ci_timeout_seconds" || key == "ci_poll_seconds" || key == "ci_max_log_bytes") {
      unsigned long long number = 0;
      const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), number);
      if (value.empty() || parse_error != std::errc{} || end != value.data() + value.size()) {
        configError(path, line_number, "expected a non-negative integer");
      }
      if (key == "ci_timeout_seconds") {
        if (number < 1 || number > 86400) configError(path, line_number, "ci_timeout_seconds must be 1 to 86400");
        config.ci_timeout_seconds = static_cast<unsigned>(number);
      } else if (key == "ci_poll_seconds") {
        if (number < 1 || number > 3600) configError(path, line_number, "ci_poll_seconds must be 1 to 3600");
        config.ci_poll_seconds = static_cast<unsigned>(number);
      } else {
        if (number < 1024 || number > (1ull << 30)) {
          configError(path, line_number, "ci_max_log_bytes must be 1024 to 1073741824");
        }
        config.ci_max_log_bytes = number;
      }
    } else if (key == "ci_artifact_retention_days" || key == "ci_artifact_max_retention_days" ||
               key == "ci_artifact_max_bytes" || key == "ci_artifact_max_project_bytes" ||
               key == "ci_artifact_max_total_bytes" || key == "ci_runs_keep" ||
               key == "ci_cleanup_interval_seconds" || key == "pages_keep_versions") {
      unsigned long long number = 0;
      const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), number);
      if (value.empty() || parse_error != std::errc{} || end != value.data() + value.size()) {
        configError(path, line_number, "expected a non-negative integer");
      }
      if (key == "ci_artifact_retention_days" || key == "ci_artifact_max_retention_days") {
        if (number < 1 || number > 3650) configError(path, line_number, "retention days must be 1 to 3650");
        if (key == "ci_artifact_retention_days") config.ci_artifact_retention_days = static_cast<unsigned>(number);
        else config.ci_artifact_max_retention_days = static_cast<unsigned>(number);
      } else if (key == "ci_artifact_max_bytes") {
        if (number < 1024) configError(path, line_number, "ci_artifact_max_bytes must be at least 1024");
        config.ci_artifact_max_bytes = number;
      } else if (key == "ci_artifact_max_project_bytes") {
        config.ci_artifact_max_project_bytes = number;  // 0 disables the per-project budget
      } else if (key == "ci_artifact_max_total_bytes") {
        config.ci_artifact_max_total_bytes = number;  // 0 disables the global budget
      } else if (key == "ci_runs_keep") {
        if (number > 1000000) configError(path, line_number, "ci_runs_keep must be 0 to 1000000");
        config.ci_runs_keep = static_cast<unsigned>(number);
      } else if (key == "pages_keep_versions") {
        if (number < 1 || number > 1000) configError(path, line_number, "pages_keep_versions must be 1 to 1000");
        config.pages_keep_versions = static_cast<unsigned>(number);
      } else {
        if (number < 60 || number > 86400) {
          configError(path, line_number, "ci_cleanup_interval_seconds must be 60 to 86400");
        }
        config.ci_cleanup_interval_seconds = static_cast<unsigned>(number);
      }
    } else if (key == "ci_allow_network" || key == "ci_artifact_keep_latest") {
      if (value != "true" && value != "false") {
        configError(path, line_number, std::string(key) + " must be true or false");
      }
      if (key == "ci_allow_network") config.ci_allow_network = value == "true";
      else config.ci_artifact_keep_latest = value == "true";
    } else if (key == "ssh_clone_target") {
      if (!isValidSshCloneTarget(value)) {
        configError(path, line_number, "ssh_clone_target must be user@host; use an SSH alias for custom ports or IPv6");
      }
      config.ssh_clone_target = value;
    } else if (key == "http_port" || key == "pages_http_port") {
      unsigned int port = 0;
      const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (value.empty() || value.size() > 5 || parse_error != std::errc{} ||
          end != value.data() + value.size() || port > 65535) {
        configError(path, line_number, std::string(key) + " must be 0 to 65535");
      }
      if (key == "http_port") config.http_port = static_cast<unsigned short>(port);
      else config.pages_http_port = static_cast<unsigned short>(port);
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
  if (config.ssh_clone_target.has_value()) {
    rendered += "ssh_clone_target=" + *config.ssh_clone_target + "\n";
  }
  if (config.ci_build_root.has_value()) {
    rendered += "ci_build_root=" + config.ci_build_root->string() + "\n";
  }
  if (config.ci_timeout_seconds.has_value()) {
    rendered += "ci_timeout_seconds=" + std::to_string(*config.ci_timeout_seconds) + "\n";
  }
  if (config.ci_max_log_bytes.has_value()) {
    rendered += "ci_max_log_bytes=" + std::to_string(*config.ci_max_log_bytes) + "\n";
  }
  if (config.ci_poll_seconds.has_value()) {
    rendered += "ci_poll_seconds=" + std::to_string(*config.ci_poll_seconds) + "\n";
  }
  if (config.ci_allow_network) {
    rendered += "ci_allow_network=true\n";
  }
  if (config.ci_artifact_retention_days.has_value()) {
    rendered += "ci_artifact_retention_days=" + std::to_string(*config.ci_artifact_retention_days) + "\n";
  }
  if (config.ci_artifact_max_retention_days.has_value()) {
    rendered += "ci_artifact_max_retention_days=" + std::to_string(*config.ci_artifact_max_retention_days) + "\n";
  }
  if (config.ci_artifact_max_bytes.has_value()) {
    rendered += "ci_artifact_max_bytes=" + std::to_string(*config.ci_artifact_max_bytes) + "\n";
  }
  if (config.ci_artifact_max_project_bytes.has_value()) {
    rendered += "ci_artifact_max_project_bytes=" + std::to_string(*config.ci_artifact_max_project_bytes) + "\n";
  }
  if (config.ci_artifact_max_total_bytes.has_value()) {
    rendered += "ci_artifact_max_total_bytes=" + std::to_string(*config.ci_artifact_max_total_bytes) + "\n";
  }
  if (config.ci_runs_keep.has_value()) {
    rendered += "ci_runs_keep=" + std::to_string(*config.ci_runs_keep) + "\n";
  }
  if (config.ci_cleanup_interval_seconds.has_value()) {
    rendered += "ci_cleanup_interval_seconds=" + std::to_string(*config.ci_cleanup_interval_seconds) + "\n";
  }
  if (config.ci_artifact_keep_latest.has_value()) {
    rendered += std::string("ci_artifact_keep_latest=") + (*config.ci_artifact_keep_latest ? "true" : "false") + "\n";
  }
  if (config.pages_root.has_value()) {
    rendered += "pages_root=" + config.pages_root->string() + "\n";
  }
  if (config.pages_http_port.has_value()) {
    rendered += "pages_http_port=" + std::to_string(*config.pages_http_port) + "\n";
  }
  if (config.pages_keep_versions.has_value()) {
    rendered += "pages_keep_versions=" + std::to_string(*config.pages_keep_versions) + "\n";
  }
  return rendered;
}

}  // namespace ckgit
