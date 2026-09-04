// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ckgit/remote_url.hpp"

namespace ckgit {

struct RefTip {
  std::string name;
  std::string object_id;
};

struct RepositoryAudit {
  std::filesystem::path path;
  std::string current_branch;
  bool detached_head{false};
  // A linked worktree shares its object store with another checkout, so it
  // is a weaker candidate for the canonical checkout of a project.
  bool linked_worktree{false};
  std::size_t changed_entries{0};
  std::vector<RefTip> branches;
  std::vector<RefTip> tags;
  std::vector<RemoteUrl> remotes;
};

struct DiscoveryResult {
  std::vector<std::filesystem::path> repositories;
  std::vector<std::string> warnings;
};

// Finds working trees below explicit roots.  The walk never follows symlinks,
// and detects both normal repositories and linked worktrees.
DiscoveryResult discoverWorkingTrees(
    const std::vector<std::filesystem::path>& roots,
    const std::vector<std::string>& exclusions = {});

// Reads Git state only.  No fetch, refresh, checkout, or configuration write is
// performed by this function.
RepositoryAudit inspectRepository(const std::filesystem::path& path);

}  // namespace ckgit
