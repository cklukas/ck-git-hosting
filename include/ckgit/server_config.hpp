// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

struct ServerConfig {
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::optional<std::filesystem::path> state_root;
  std::optional<std::filesystem::path> hook_directory;
  std::optional<unsigned short> http_port;
  // Public SSH destination used in copyable dashboard clone commands. It is
  // deliberately independent of the loopback HTTP listener or request Host.
  std::optional<std::string> ssh_clone_target{};
};

// Loads the strict, bounded version-1 daemon configuration.  Every path must
// be absolute; unknown fields, duplicate fields, invalid UTF-8, and malformed
// values fail closed.  No path is opened here: the daemon validates each one
// when it starts serving, and `--check` reports the parsed values only.
ServerConfig loadServerConfig(const std::filesystem::path& path);

// Accepts a shell-safe user@host destination. SSH aliases can supply ports,
// IPv6 addresses, keys, and other transport settings in the user's SSH config.
bool isValidSshCloneTarget(std::string_view value);

// Renders the effective configuration in the same key=value form.
std::string renderServerConfig(const ServerConfig& config);

}  // namespace ckgit
