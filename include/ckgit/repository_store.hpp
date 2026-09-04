// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

// Verifies a configured repository root without accepting a symlink root.
std::filesystem::path validatedRepositoryRoot(const std::filesystem::path& root);

// Forms a repository path only from a validated first-release project name.
std::filesystem::path bareRepositoryPath(const std::filesystem::path& root,
                                         std::string_view project_name);

// Creates a standard bare repository with the product's receive safety
// settings.  The caller must provision the root with its service-account
// ownership and mode.  A dry run validates and returns the proposed path only.
std::filesystem::path createBareRepository(const std::filesystem::path& root,
                                           std::string_view project_name,
                                           std::string_view default_branch,
                                           bool dry_run = false,
                                           const std::optional<std::filesystem::path>& hook_directory = std::nullopt);

}  // namespace ckgit
