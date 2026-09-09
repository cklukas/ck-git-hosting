// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

struct RecoveryPaths {
  std::filesystem::path repo_root;
  std::filesystem::path state_root;
  std::optional<std::filesystem::path> hook_directory;
};

struct BackupSummary {
  std::vector<std::string> projects;
  std::size_t files{};
  std::uintmax_t bytes{};
};

// Backups contain reachable Git objects and every hosted ref/HEAD, separately
// preserved original Git configuration, and private metadata. Reflogs,
// unreachable objects, operating-system configuration, and keys are excluded.
BackupSummary backupHostedRepositories(const RecoveryPaths& paths,
                                      const std::filesystem::path& output, bool dry_run = false);
BackupSummary verifyBackup(const std::filesystem::path& backup);

// Full recovery requires empty target metadata and absent project destinations.
// Target root directories retain their inodes and cross-process locks.
std::vector<std::string> restoreBackup(const RecoveryPaths& paths,
                                     const std::filesystem::path& backup, bool dry_run = false);

struct TrashedRepository {
  std::string entry;
  std::string project;
};
std::vector<TrashedRepository> listTrashedRepositories(const RecoveryPaths& paths);

// Restore Git only, using fresh hosting settings and target hooks. The retained
// trash entry is kept as a recovery copy; removed metadata is never recreated.
std::filesystem::path restoreTrashedRepository(const RecoveryPaths& paths, std::string_view entry,
                                              const std::optional<std::string>& name = std::nullopt,
                                              bool dry_run = false);

}  // namespace ckgit
