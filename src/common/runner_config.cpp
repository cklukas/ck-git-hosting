// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/runner_config.hpp"
#include "ckgit/ci_runner_protocol.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/text.hpp"
#include "ckgit/path_utf8.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
namespace ckgit {
namespace {
[[noreturn]] void configError(const std::filesystem::path& path, std::size_t line, const std::string& message) {
  throw std::runtime_error("invalid runner configuration " + path.string() + ":" + std::to_string(line) + ": " +
                           message);
}

#ifndef _WIN32
bool isShellSafe(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                  c == '.' || c == '_' || c == '-' || c == '@' || c == ':' || c == '+' || c == '=';
         });
}
#endif
std::string pathText(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
}  // namespace
// --- configuration -----------------------------------------------------------

RunnerConfig loadRunnerConfig(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::file_size(path, error) > 64 * 1024) {
    throw std::runtime_error("runner configuration must be a small regular file: " + path.string());
  }
  std::ifstream file(path, std::ios::binary);
  RunnerConfig config;
  std::unordered_set<std::string> seen;
  bool schema = false;
  std::size_t number = 0;
  const auto absolute = [&](const std::string& value) {
    if (value.empty() || !pathFromUtf8(value).is_absolute() || value.size() > 1024 || hasControlCharacter(value)) {
      configError(path, number, "expected an absolute path");
    }
    return pathFromUtf8(value);
  };
  const auto boolean = [&](const std::string& value) {
    if (value != "true" && value != "false") configError(path, number, "expected true or false");
    return value == "true";
  };
  const auto integer = [&](const std::string& value, unsigned low, unsigned high) {
    unsigned parsed = 0;
    const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || parse_error != std::errc{} || end != value.data() + value.size() || parsed < low ||
        parsed > high) {
      configError(path, number, "expected a number from " + std::to_string(low) + " to " + std::to_string(high));
    }
    return parsed;
  };
  for (std::string line; std::getline(file, line);) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 2048) configError(path, number, "line too long");
    if (line.empty() || line.front() == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0) configError(path, number, "expected key=value");
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (!seen.insert(key).second) configError(path, number, "duplicate field");
    if (key == "schema_version") {
      if (value != "1") configError(path, number, "unsupported schema_version");
      schema = true;
    } else if (key == "server") {
      if (!isValidSshCloneTarget(value)) configError(path, number, "server must be user@host");
      config.server = value;
    } else if (key == "name") {
      if (!isValidClientId(value) || value == kCiPrimaryRunnerName) configError(path, number, "invalid runner name");
      config.name = value;
    } else if (key == "ci_build_root") {
      config.build_root = absolute(value);
    } else if (key == "identity") {
      config.identity = absolute(value);
    } else if (key == "known_hosts") {
      config.known_hosts = absolute(value);
    } else if (key == "ssh_program") {
      config.ssh_program = absolute(value);
    } else if (key == "ci_cache_root") {
      config.cache_root = absolute(value);
    } else if (key == "ci_toolchain_root") {
      config.toolchain_root = absolute(value);
    } else if (key == "ci_toolchain_paths") {
      config.toolchain_paths.clear();
      std::size_t start = 0;
      do {
        const auto separator = value.find(';', start);
        const auto part = value.substr(start, separator == std::string::npos ? separator : separator - start);
        const auto relative = pathFromUtf8(part);
        if (part.empty() || part.find(':') != std::string::npos || part.find('\\') != std::string::npos ||
            hasControlCharacter(part) || relative.is_absolute() || relative.has_root_path() ||
            std::any_of(relative.begin(), relative.end(), [](const auto& item) { return item == ".."; }))
          configError(path, number, "toolchain paths must be relative slash-separated directories");
        config.toolchain_paths.push_back(relative);
        if (config.toolchain_paths.size() > 32) configError(path, number, "too many toolchain directories");
        if (separator == std::string::npos) break;
        start = separator + 1;
      } while (true);
    } else if (key.starts_with("ci_toolchain_env.")) {
      const auto name = key.substr(std::string_view("ci_toolchain_env.").size());
      if (name.empty() || name.size() > 128 || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos ||
          (name.front() >= '0' && name.front() <= '9') || hasControlCharacter(value))
        configError(path, number, "invalid uppercase toolchain environment assignment");
      config.toolchain_environment[name] = value;
    } else if (key == "claim_wait_seconds") {
      config.claim_wait_seconds = integer(value, 1, kMaximumCiClaimWaitSeconds);
    } else if (key == "ci_timeout_seconds") {
      config.timeout_seconds = integer(value, 1, 86400);
    } else if (key == "ci_allow_network") {
      config.allow_network = boolean(value);
    } else if (key == "ci_allow_degraded") {
      config.allow_degraded = boolean(value);
#ifdef _WIN32
      if (config.allow_degraded) configError(path, number, "Windows jobs require AppContainer isolation");
#endif
    } else {
      configError(path, number, "unknown field");
    }
  }
  if (!schema || config.server.empty() || config.name.empty() || config.build_root.empty()) {
    throw std::runtime_error("runner configuration needs schema_version, server, name and ci_build_root: " +
                             path.string());
  }
#ifndef _WIN32
  for (const std::filesystem::path* used : {&config.build_root, &config.ssh_program}) {
    if (!isShellSafe(used->string())) throw std::runtime_error("runner paths may use only [A-Za-z0-9/._-]");
  }
#endif
  return config;
}

std::string renderRunnerConfig(const RunnerConfig& config) {
  std::string out = "schema_version=1\nserver=" + config.server + "\nname=" + config.name +
                    "\nci_build_root=" + pathText(config.build_root) + "\n";
  if (config.identity) out += "identity=" + pathText(*config.identity) + "\n";
  if (config.known_hosts) out += "known_hosts=" + pathText(*config.known_hosts) + "\n";
  if (config.ssh_program != RunnerConfig{}.ssh_program) out += "ssh_program=" + pathText(config.ssh_program) + "\n";
  if (config.claim_wait_seconds != 60) out += "claim_wait_seconds=" + std::to_string(config.claim_wait_seconds) + "\n";
  if (config.cache_root) out += "ci_cache_root=" + pathText(*config.cache_root) + "\n";
  if (!config.allow_network) out += "ci_allow_network=false\n";
  if (config.timeout_seconds) out += "ci_timeout_seconds=" + std::to_string(*config.timeout_seconds) + "\n";
  if (config.allow_degraded) out += "ci_allow_degraded=true\n";
  if (config.toolchain_root) out += "ci_toolchain_root=" + pathText(*config.toolchain_root) + "\n";
  if (config.toolchain_paths != RunnerConfig{}.toolchain_paths) {
    out += "ci_toolchain_paths=";
    for (std::size_t i = 0; i < config.toolchain_paths.size(); ++i) {
      if (i) out += ';';
      const auto raw = config.toolchain_paths[i].generic_u8string();
      out.append(raw.begin(), raw.end());
    }
    out += '\n';
  }
  for (const auto& [name, value] : config.toolchain_environment) out += "ci_toolchain_env." + name + "=" + value + "\n";
  return out;
}

}  // namespace ckgit
