// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ckgit/metadata_store.hpp"

namespace ckgit {

struct ProjectSummary {
  std::string name;
  std::string default_branch;
  bool valid_head{false};
  std::size_t branch_count{0};
  std::size_t tag_count{0};
  std::vector<CheckoutMetadata> checkouts;
  std::vector<StateEvent> events;
};

// Inspects immediate standard bare repositories beneath a validated root. It
// never changes a ref or configuration and ignores entries outside the project
// naming grammar.
std::vector<ProjectSummary> inspectHostedProjects(
    const std::filesystem::path& repo_root,
    const std::optional<std::filesystem::path>& metadata_root = std::nullopt);

}  // namespace ckgit
