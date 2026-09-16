// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ckgit/ci_store.hpp"
#include "ckgit/metadata_store.hpp"

namespace ckgit {

struct CommitSummary {
  std::string id;
  std::string author;
  std::string subject;
  std::uint64_t epoch_seconds{};
};

struct ProjectRef {
  // Short name without refs/heads/ or refs/tags/. is_tag disambiguates names
  // that exist in both namespaces.
  std::string name;
  std::string id;
  std::uint64_t epoch_seconds{};
  bool is_tag{false};
};

struct ProjectSummary {
  std::string name;
  std::string default_branch;
  bool valid_head{false};
  std::size_t branch_count{0};
  std::size_t tag_count{0};
  // Committer time of the newest commit reachable from any branch or tag;
  // zero for an empty repository.
  std::uint64_t last_commit_epoch_seconds{0};
  std::vector<CheckoutMetadata> checkouts;
  std::vector<StateEvent> events;
  // Newest-first CI run records (status, timing, per-step results). Loaded on
  // the metadata-revision channel, since CI status changes without a ref move.
  std::vector<CiRunRecord> ci_runs{};
  // Newest-first durable releases (tag + assets), loaded on the same channel.
  std::vector<CiReleaseRecord> releases{};
  std::optional<CommitSummary> last_commit{};
  std::string head_id{};
  std::uint64_t size_bytes{};
  std::uint64_t object_count{};
  std::vector<ProjectRef> refs{};
  // UTC calendar dates, populated from HEAD (normally the default branch).
  std::map<std::string, std::size_t> activity{};
  std::string readme_path{};
  std::string readme_content{};
  std::uint64_t fingerprint{};
  std::uint64_t generated_epoch_seconds{};
  bool indexing{false};
  bool history_truncated{false};
  bool refs_truncated{false};
  bool readme_truncated{false};
  std::string index_error{};
  // Presentation-only: the HTTP server attaches its configured public SSH
  // destination to the copied snapshot. The project index does not persist it.
  std::string ssh_clone_target{};
};

// Inspects immediate standard bare repositories beneath a validated root. It
// never changes a ref or configuration and ignores entries outside the project
// naming grammar.
std::vector<ProjectSummary> inspectHostedProjects(
    const std::filesystem::path& repo_root,
    const std::optional<std::filesystem::path>& metadata_root = std::nullopt);

}  // namespace ckgit
