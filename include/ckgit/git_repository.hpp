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
  // True when the worktree status was larger than the bounded capture; the
  // count is then a lower bound rather than exact.
  bool changed_entries_truncated{false};
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

// Finds only the requested folder's immediate child working trees. Symlinked
// children are skipped, and malformed or unreadable candidates are warnings.
DiscoveryResult discoverImmediateWorkingTrees(const std::filesystem::path& folder);

// True when this directory or an ancestor contains Git repository metadata,
// including a bare repository or a broken .git entry. This deliberately keeps
// a damaged checkout from being mistaken for a folder to publish in bulk.
// An unavailable or non-directory path is an error.
bool hasRepositoryContext(const std::filesystem::path& path);

// Reads Git state only.  No fetch, refresh, checkout, or configuration write is
// performed by this function.  Like Git itself, a path inside a working tree
// resolves to that tree's root.
RepositoryAudit inspectRepository(const std::filesystem::path& path);

// For discovered candidates: the path must be the root of its own working
// tree.  A stale `.git` entry inside another repository would otherwise be
// reported as a second checkout of the surrounding project.
RepositoryAudit inspectDiscoveredRepository(const std::filesystem::path& path);

}  // namespace ckgit
