// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_dependency_lock.hpp"
namespace ckgit {
struct CiDependencyDecision {
  std::uint64_t sequence = 0;
  std::uint64_t epoch = 0;
  std::uint64_t effective_uid = 0;  // service identity, not necessarily the human operator
  std::string package;
  std::string action;  // approve, reject, revoke
};
struct CiDependencyRequest {
  CiDependencyLock lock;
  std::vector<CiDependencyDecision> history;
  std::vector<std::string> commits;
};
// Review status only. This never implies provisioning or authorizes a job.
std::string_view ciDependencyApprovalStatus(const CiDependencyRequest& request);
// These operations manage review state only; they do not provision or release jobs.
std::string registerCiDependencyRequest(const std::filesystem::path& state_root, std::string_view project,
                                        std::string_view lock_bytes, std::string_view commit = {});
void decideCiDependency(const std::filesystem::path& state_root, std::string_view project,
                        std::string_view digest, std::string_view package, std::string_view action);
std::vector<CiDependencyRequest> listCiDependencyRequests(const std::filesystem::path& state_root,
                                                          std::string_view project,
                                                          std::size_t limit = 1024,
                                                          bool* truncated = nullptr);
// Read one exact review record without the list limit. This is a snapshot for
// display; it does not authorize provisioning or a job and may be revoked.
std::optional<CiDependencyRequest> getCiDependencyRequest(const std::filesystem::path& state_root,
                                                           std::string_view project, std::string_view digest);
// Execute a short server-side admission transaction while holding the same
// request lock used by decisions. This verifies review and the recorded commit
// and target only; callers must separately verify source inputs and a protected,
// exact provisioned inventory before admitting work. Do not run the job or
// call dependency-store operations from inside the callback.
void withApprovedCiDependencyReview(const std::filesystem::path& state_root, std::string_view project,
                                    std::string_view digest, std::string_view commit,
                                    std::string_view platform, std::string_view architecture,
                                    const std::function<void(const CiDependencyRequest&)>& transaction);
}  // namespace ckgit
