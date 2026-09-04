// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/repository_store.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/process.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

void runGitOrThrow(const std::vector<std::string>& command, std::string_view action) {
  const ProcessResult result = runProcess(command);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("Git could not " + std::string(action) + ": " + result.output);
  }
}

class ScopedUmask {
 public:
  ScopedUmask() : old_mask_(umask(0027)) {}
  ~ScopedUmask() { umask(old_mask_); }

  ScopedUmask(const ScopedUmask&) = delete;
  ScopedUmask& operator=(const ScopedUmask&) = delete;

 private:
  mode_t old_mask_;
};

std::optional<std::filesystem::path> validatedHookDirectory(
    const std::optional<std::filesystem::path>& requested_directory) {
  if (!requested_directory.has_value()) {
    return std::nullopt;
  }
  std::error_code error;
  const auto directory_status = std::filesystem::symlink_status(*requested_directory, error);
  if (error || !std::filesystem::is_directory(directory_status) ||
      std::filesystem::is_symlink(directory_status)) {
    throw std::invalid_argument("hook directory must be an existing non-symlink directory");
  }
  const auto directory = std::filesystem::canonical(*requested_directory, error);
  if (error) {
    throw std::invalid_argument("hook directory cannot be resolved");
  }
  const auto hook = directory / "post-receive";
  const auto hook_status = std::filesystem::symlink_status(hook, error);
  if (error || !std::filesystem::is_regular_file(hook_status) || std::filesystem::is_symlink(hook_status) ||
      access(hook.c_str(), X_OK) != 0) {
    throw std::invalid_argument("hook directory must contain an executable non-symlink post-receive hook");
  }
  return directory;
}

}  // namespace

std::filesystem::path validatedRepositoryRoot(const std::filesystem::path& root) {
  if (root.empty()) {
    throw std::invalid_argument("repository root is required");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    throw std::invalid_argument("repository root must be an existing non-symlink directory");
  }
  const auto canonical = std::filesystem::canonical(root, error);
  if (error) {
    throw std::invalid_argument("repository root cannot be resolved");
  }
  return canonical;
}

std::filesystem::path bareRepositoryPath(const std::filesystem::path& root,
                                         std::string_view project_name) {
  if (!isValidProjectName(project_name)) {
    throw std::invalid_argument("invalid project name: " + projectNameError(project_name));
  }
  return validatedRepositoryRoot(root) / (std::string(project_name) + ".git");
}

std::filesystem::path createBareRepository(const std::filesystem::path& root,
                                           std::string_view project_name,
                                           std::string_view default_branch,
                                           bool dry_run,
                                           const std::optional<std::filesystem::path>& hook_directory) {
  const auto repository = bareRepositoryPath(root, project_name);
  if (!isValidBranchName(default_branch)) {
    throw std::invalid_argument("invalid default branch");
  }
  std::error_code error;
  const auto existing = std::filesystem::symlink_status(repository, error);
  const bool absent = existing.type() == std::filesystem::file_type::not_found ||
                      error == std::errc::no_such_file_or_directory;
  if (!absent) {
    throw std::runtime_error("repository already exists or cannot be inspected: " + repository.string());
  }
  const auto hooks = validatedHookDirectory(hook_directory);
  if (dry_run) {
    return repository;
  }

  ScopedUmask restrictive_umask;
  runGitOrThrow({"git", "init", "--bare", "--initial-branch=" + std::string(default_branch),
                 repository.string()},
                "create bare repository");
  if (chmod(repository.c_str(), 0750) != 0) {
    throw std::runtime_error("could not set repository mode: " +
                             std::string(std::strerror(errno)));
  }
  runGitOrThrow({"git", "-C", repository.string(), "config", "receive.denyDeletes", "true"},
                "configure receive.denyDeletes");
  runGitOrThrow({"git", "-C", repository.string(), "config", "receive.denyNonFastForwards", "true"},
                "configure receive.denyNonFastForwards");
  runGitOrThrow({"git", "-C", repository.string(), "config", "receive.advertisePushOptions", "false"},
                "disable receive push options");
  if (hooks.has_value()) {
    runGitOrThrow({"git", "-C", repository.string(), "config", "core.hooksPath", hooks->string()},
                  "configure post-receive hook path");
  }
  return repository;
}

}  // namespace ckgit
