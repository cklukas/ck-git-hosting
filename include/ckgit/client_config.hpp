// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ckgit {

struct ClientConfig {
  unsigned int schema_version{1};
  std::string client_id;
  std::string display_name;
  std::string server;
  std::string remote_name;
  std::vector<std::filesystem::path> scan_roots;
  std::vector<std::string> exclusions;
  bool expose_full_paths{false};
  // Absolute directory of the loaded file.  The client keeps its sync lock
  // and canonical checkout selection beside the configuration that owns them.
  std::filesystem::path config_directory;
  // Dashboard forwarding uses an ordinary SSH login, separate from the
  // restricted Git account. An empty host keeps the legacy host-only default.
  std::string web_host{};
  unsigned short web_port{8420};
};

// Loads the strict, bounded version-1 client configuration.  Unknown fields,
// duplicate singleton fields, invalid UTF-8, and malformed values fail closed.
ClientConfig loadClientConfig(const std::filesystem::path& path);

// Serialize validated values without touching the filesystem. Saves create
// private parent directories as needed and install a complete mode-0600 file
// atomically. Existing files are retained unless overwrite is explicitly true.
std::string renderClientConfig(const ClientConfig& config);
void saveClientConfig(const std::filesystem::path& path, const ClientConfig& config,
                      bool overwrite = false);

}  // namespace ckgit
