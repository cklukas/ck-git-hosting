// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/repository_store.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#include "ckgit/process.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

// Repository lifecycle and registration share one cross-process gate. Always
// acquire it before the metadata-state lock, and never while holding an index
// mutex. The orphan check and cleanup must be one operation relative to create.
class RepositoryMutationLock {
 public:
  explicit RepositoryMutationLock(const std::filesystem::path& root) {
    descriptor_ = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor_ < 0) throw std::runtime_error("could not open repository lifecycle lock");
    while (flock(descriptor_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        close(descriptor_);
        throw std::runtime_error("could not acquire repository lifecycle lock");
      }
    }
  }
  ~RepositoryMutationLock() { close(descriptor_); }
  RepositoryMutationLock(const RepositoryMutationLock&) = delete;
  RepositoryMutationLock& operator=(const RepositoryMutationLock&) = delete;
 private:
  int descriptor_ = -1;
};

void runGitOrThrow(const std::vector<std::string>& command, std::string_view action) {
  const ProcessResult result = runProcess(command, std::chrono::seconds(60));
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
  const RepositoryMutationLock lifecycle(repository.parent_path());
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

std::filesystem::path removeProject(const std::filesystem::path& repo_root,
                                    const std::filesystem::path& state_root,
                                    std::string_view project_name, bool dry_run) {
  const auto root = validatedRepositoryRoot(repo_root);
  const RepositoryMutationLock lifecycle(root);
  const auto repository = bareRepositoryPath(root, project_name);
  struct stat repository_status {};
  if (lstat(repository.c_str(), &repository_status) != 0 || !S_ISDIR(repository_status.st_mode)) {
    throw std::runtime_error("repository to remove must be an existing non-symlink directory");
  }
  // Check metadata and trash before moving the repository, so unsafe state
  // does not turn an ordinary validation failure into a partial removal.
  removeProjectMetadata(state_root, project_name, true);
  const auto trash = root / ".trash";
  struct stat trash_status {};
  if (lstat(trash.c_str(), &trash_status) == 0) {
    if (!S_ISDIR(trash_status.st_mode) || trash_status.st_uid != geteuid() ||
        (trash_status.st_mode & 0022) != 0) {
      throw std::runtime_error("repository trash must be a non-symlink directory owned by the current user");
    }
  } else if (errno != ENOENT) {
    throw std::runtime_error("could not inspect repository trash");
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const std::string prefix = std::string(project_name) + ".git." + std::to_string(seconds);
  std::filesystem::path destination;
  for (unsigned sequence = 0; sequence < 1000000; ++sequence) {
    destination = trash / (prefix + (sequence == 0 ? std::string{} : "." + std::to_string(sequence)));
    struct stat existing {};
    if (lstat(destination.c_str(), &existing) != 0) {
      if (errno != ENOENT) throw std::runtime_error("could not inspect repository trash destination");
      break;
    }
    destination.clear();
  }
  if (destination.empty()) throw std::runtime_error("repository trash sequence exhausted");
  if (dry_run) return destination;
  if (mkdir(trash.c_str(), 0700) != 0 && errno != EEXIST) {
    throw std::runtime_error("could not create repository trash");
  }
  const int root_descriptor = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const int trash_descriptor = root_descriptor < 0 ? -1 : openat(root_descriptor, ".trash",
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root_descriptor < 0 || trash_descriptor < 0) {
    if (root_descriptor >= 0) close(root_descriptor);
    throw std::runtime_error("could not open repository trash");
  }
  try {
    struct stat checked {};
    if (fstat(trash_descriptor, &checked) != 0 || checked.st_uid != geteuid() ||
        (checked.st_mode & 0022) != 0) throw std::runtime_error("repository trash is unsafe");
    const std::string repository_name = repository.filename().string();
    if (fstatat(root_descriptor, repository_name.c_str(), &checked, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(checked.st_mode) || checked.st_dev != repository_status.st_dev ||
        checked.st_ino != repository_status.st_ino) {
      throw std::runtime_error("repository changed while preparing removal");
    }
    const std::string destination_name = destination.filename().string();
    // Reserve the destination so concurrent removers cannot choose the same
    // name. The directory becomes the retained repository after the rename.
    if (mkdirat(trash_descriptor, destination_name.c_str(), 0700) != 0) {
      throw std::runtime_error("repository trash destination already exists");
    }
    if (renameat(root_descriptor, repository_name.c_str(), trash_descriptor, destination_name.c_str()) != 0) {
      unlinkat(trash_descriptor, destination_name.c_str(), AT_REMOVEDIR);
      throw std::runtime_error("could not move repository to trash");
    }
    if (fsync(root_descriptor) != 0 || fsync(trash_descriptor) != 0) {
      throw std::runtime_error("could not sync repository removal");
    }
    close(trash_descriptor);
    close(root_descriptor);
  } catch (...) {
    close(trash_descriptor);
    close(root_descriptor);
    throw;
  }
  removeProjectMetadata(state_root, project_name);
  return destination;
}

std::vector<std::string> sweepOrphanProjectMetadata(const std::filesystem::path& repo_root,
                                                    const std::filesystem::path& state_root) {
  const auto root = validatedRepositoryRoot(repo_root);
  const RepositoryMutationLock lifecycle(root);
  std::vector<std::string> removed;
  for (const auto& name : listProjectMetadataNames(state_root)) {
    struct stat status {};
    const auto repository = bareRepositoryPath(root, name);
    const int inspected = lstat(repository.c_str(), &status);
    if (inspected == 0 && S_ISDIR(status.st_mode)) continue;
    if (inspected != 0 && errno != ENOENT) throw std::runtime_error("could not inspect orphan repository");
    removeProjectMetadata(state_root, name);
    removed.push_back(name);
  }
  return removed;
}

void registerHostedCheckout(const std::filesystem::path& repo_root,
                             const std::filesystem::path& state_root,
                             std::string_view project_name, std::string_view client_id,
                             std::string_view encoded_path, bool replace) {
  const auto repository = bareRepositoryPath(repo_root, project_name);
  const RepositoryMutationLock lifecycle(repository.parent_path());
  struct stat status {};
  if (lstat(repository.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
    throw std::runtime_error("cannot register a checkout for an unavailable repository");
  }
  registerCheckout(state_root, project_name, client_id, encoded_path, replace);
  appendStateEvent(state_root, "checkout-registered", project_name, client_id);
}

bool forgetHostedCheckout(const std::filesystem::path& repo_root,
                          const std::filesystem::path& state_root,
                          std::string_view project_name, std::string_view client_id) {
  const auto root = validatedRepositoryRoot(repo_root);
  const RepositoryMutationLock lifecycle(root);
  // Forgetting remains useful after a repository was removed. It only removes
  // this client's existing report and cannot recreate any project metadata.
  return forgetCheckout(state_root, project_name, client_id);
}

bool appendHostedStateEvent(const std::filesystem::path& repo_root,
                            const std::filesystem::path& state_root,
                            std::string_view kind, std::string_view project_name,
                            std::string_view client_id) {
  const auto repository = bareRepositoryPath(repo_root, project_name);
  const RepositoryMutationLock lifecycle(repository.parent_path());
  struct stat status {};
  if (lstat(repository.c_str(), &status) != 0) {
    if (errno == ENOENT) return false;
    throw std::runtime_error("could not inspect repository before recording event");
  }
  if (!S_ISDIR(status.st_mode)) return false;
  appendStateEvent(state_root, kind, project_name, client_id);
  return true;
}

}  // namespace ckgit
