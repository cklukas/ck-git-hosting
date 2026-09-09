// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// Renames a repository into durable repo_root/.trash and removes all project
// metadata. Never follows a repository, trash-directory, or metadata symlink.
// The returned path is the retained bare repository; --dry-run only validates.
std::filesystem::path removeProject(const std::filesystem::path& repo_root,
                                    const std::filesystem::path& state_root,
                                    std::string_view project_name, bool dry_run = false);

// Cleans metadata for repositories deleted outside the administrator command.
// Called by the index sweep; returns the project names that were cleaned.
std::vector<std::string> sweepOrphanProjectMetadata(const std::filesystem::path& repo_root,
                                                    const std::filesystem::path& state_root);

// Register and record the event while holding the repository lifecycle lock.
// The repository is rechecked inside that lock, so removal/sweep cannot race
// a successful registration into leaving or erasing metadata unexpectedly.
void registerHostedCheckout(const std::filesystem::path& repo_root,
                             const std::filesystem::path& state_root,
                             std::string_view project_name, std::string_view client_id,
                             std::string_view encoded_path, bool replace = false);

bool forgetHostedCheckout(const std::filesystem::path& repo_root,
                          const std::filesystem::path& state_root,
                          std::string_view project_name, std::string_view client_id);

// Ignore an event for a repository that was removed while a push or creation
// was finishing. Returns false if the repository is no longer available.
bool appendHostedStateEvent(const std::filesystem::path& repo_root,
                            const std::filesystem::path& state_root,
                            std::string_view kind, std::string_view project_name,
                            std::string_view client_id);

}  // namespace ckgit
