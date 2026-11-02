// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "ckgit/ci_dependency_lock.hpp"

namespace ckgit {
struct CiDependencyCommitLock {
  CiDependencyLock lock;
  std::string bytes;
};
// Read an exact commit in an operator-controlled bare repository. The lock and
// all declared inputs must be regular Git files, bounded and hash-consistent.
// Reading a valid lock is not approval and does not authorize a CI job.
CiDependencyCommitLock readCiDependencyCommitLock(const std::filesystem::path& repository,
                                                  std::string_view commit, std::string_view lock_path);
// Verify the lock and its source inputs from an exact Git commit, then hold
// the matching approval lock throughout a short transaction. This does not
// validate a provisioned toolchain or, by itself, authorize CI execution.
void withReviewedCiDependencyCommit(const std::filesystem::path& state_root, std::string_view project,
                                    const std::filesystem::path& repository, std::string_view commit,
                                    std::string_view lock_path, std::string_view expected_digest,
                                    std::string_view platform, std::string_view architecture,
                                    const std::function<void(const CiDependencyCommitLock&)>& transaction);
}  // namespace ckgit
