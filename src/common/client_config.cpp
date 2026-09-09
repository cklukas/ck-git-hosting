// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/client_config.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

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
  return user.front() != '-' && host.front() != '-' &&
         std::all_of(user.begin(), user.end(), [](unsigned char character) {
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

bool isValidWebHost(std::string_view value) {
  if (value.empty() || value.size() > 255) return false;
  const auto at = value.find('@');
  const auto safe = [](std::string_view part, bool host) {
    return !part.empty() && part.front() != '-' &&
        std::all_of(part.begin(), part.end(), [host](unsigned char c) {
          return std::isalnum(c) != 0 || c == '-' || c == '_' || (host && c == '.');
        });
  };
  return (at == std::string_view::npos || safe(value.substr(0, at), false)) &&
      safe(value.substr(at == std::string_view::npos ? 0 : at + 1), true);
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
    } else if (key == "web_host") {
      if (!isValidWebHost(value)) configError(path, line_number, "invalid web_host; expected an SSH alias or user@host");
      config.web_host = value;
    } else if (key == "web_port") {
      unsigned int port = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535)
        configError(path, line_number, "web_port must be between 1 and 65535");
      config.web_port = static_cast<unsigned short>(port);
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

std::string renderClientConfig(const ClientConfig& config) {
  if (config.schema_version != 1) throw std::invalid_argument("schema_version must be 1");
  if (!isValidClientId(config.client_id)) throw std::invalid_argument("client_id must be a valid device identifier");
  if (!isSafeDisplayName(config.display_name)) throw std::invalid_argument("display_name must contain 1 to 128 printable UTF-8 bytes");
  if (!isValidServer(config.server)) throw std::invalid_argument("server must be user@host (use an SSH alias for custom ports)");
  if (!isValidRemoteName(config.remote_name)) throw std::invalid_argument("remote_name must be a valid Git remote name");
  if (!config.web_host.empty() && !isValidWebHost(config.web_host)) throw std::invalid_argument("web_host must be an SSH alias or user@host");
  if (config.web_port == 0) throw std::invalid_argument("web_port must be between 1 and 65535");
  if (config.scan_roots.size() > kMaximumListEntries || config.exclusions.size() > kMaximumListEntries)
    throw std::invalid_argument("configuration allows at most 64 scan roots and 64 exclusions");
  std::string output;
  const auto add = [&](std::string_view key, const std::string& value) {
    if (value.empty() || value.find('=') != std::string::npos || hasControlCharacter(value) ||
        hasOuterWhitespace(value) || !isValidUtf8(value) || key.size() + value.size() + 1 > kMaximumLineBytes)
      throw std::invalid_argument(std::string(key) + " contains an empty, unsafe, or oversized value");
    output += std::string(key) + "=" + value + "\n";
    if (output.size() > kMaximumConfigBytes) throw std::invalid_argument("client configuration exceeds 64 KiB");
  };
  add("schema_version", "1"); add("client_id", config.client_id); add("display_name", config.display_name);
  add("server", config.server); add("remote_name", config.remote_name);
  add("public_path_mode", config.expose_full_paths ? "full" : "basename");
  if (!config.web_host.empty()) add("web_host", config.web_host);
  add("web_port", std::to_string(config.web_port));
  for (const auto& root : config.scan_roots) {
    if (!root.is_absolute()) throw std::invalid_argument("scan_root must be an absolute path");
    add("scan_root", root.string());
  }
  for (const auto& pattern : config.exclusions) {
    if (pattern.size() > 256) throw std::invalid_argument("exclude pattern exceeds 256 bytes");
    add("exclude", pattern);
  }
  return output;
}

void saveClientConfig(const std::filesystem::path& requested, const ClientConfig& config, bool overwrite) {
  const auto content = renderClientConfig(config);  // Validate before creating any directory.
  if (requested.empty() || requested.filename().empty()) throw std::invalid_argument("configuration file path is required");
  const auto path = std::filesystem::absolute(requested).lexically_normal();
  auto directory = path.root_path();
  for (const auto& component : path.parent_path().relative_path()) {
    directory /= component;
    std::error_code error;
    auto status = std::filesystem::symlink_status(directory, error);
    if (status.type() == std::filesystem::file_type::not_found || error == std::errc::no_such_file_or_directory) {
      if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create private configuration directory: " + directory.string());
      status = std::filesystem::symlink_status(directory, error);
    }
    if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
      throw std::runtime_error("configuration parent must be a non-symlink directory: " + directory.string());
  }
  struct stat previous{};
  const bool existed = lstat(path.c_str(), &previous) == 0;
  if (!existed && errno != ENOENT) throw std::runtime_error("cannot inspect configuration: " + path.string());
  if (existed && (!S_ISREG(previous.st_mode) || previous.st_uid != geteuid()))
    throw std::runtime_error("configuration must be a regular file owned by this user: " + path.string());
  if (existed && !overwrite) throw std::runtime_error("configuration already exists; use --overwrite to replace it: " + path.string());
  static std::atomic<unsigned long> sequence{0};
  std::filesystem::path staging;
  int descriptor = -1;
  for (int attempt = 0; attempt < 64 && descriptor < 0; ++attempt) {
    staging = path.parent_path() / ("." + path.filename().string() + ".new." + std::to_string(getpid()) + "." + std::to_string(sequence++));
    descriptor = open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0 && errno != EEXIST) throw std::runtime_error("cannot stage client configuration: " + std::string(std::strerror(errno)));
  }
  if (descriptor < 0) throw std::runtime_error("could not choose a private configuration staging file");
  try {
    std::size_t written = 0;
    while (written < content.size()) {
      const auto count = write(descriptor, content.data() + written, content.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("could not write client configuration");
      written += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0) throw std::runtime_error("could not flush client configuration");
    close(descriptor); descriptor = -1;
    if (existed) {
      struct stat current{};
      if (lstat(path.c_str(), &current) != 0 || current.st_dev != previous.st_dev || current.st_ino != previous.st_ino ||
          current.st_size != previous.st_size || current.st_mtime != previous.st_mtime)
        throw std::runtime_error("configuration changed while it was being saved; review it and retry");
      if (rename(staging.c_str(), path.c_str()) != 0) throw std::runtime_error("could not replace client configuration");
    } else {
      // link refuses a file created by another process after the initial check.
      if (link(staging.c_str(), path.c_str()) != 0) throw std::runtime_error("could not install configuration without replacing an existing file");
      unlink(staging.c_str());
    }
  } catch (...) {
    if (descriptor >= 0) close(descriptor);
    unlink(staging.c_str());
    throw;
  }
}

}  // namespace ckgit
