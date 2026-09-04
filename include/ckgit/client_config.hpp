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
};

// Loads the strict, bounded version-1 client configuration.  Unknown fields,
// duplicate singleton fields, invalid UTF-8, and malformed values fail closed.
ClientConfig loadClientConfig(const std::filesystem::path& path);

}  // namespace ckgit
