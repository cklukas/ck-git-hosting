// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_dependency_commit.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "ckgit/process.hpp"
#include "ckgit/ci_dependency_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
CiDependencyCommitLock readCiDependencyCommitLock(const std::filesystem::path& repository,
                                                  std::string_view commit, std::string_view lock_path) {
  const auto printable = [](std::string_view path) {
    return std::all_of(path.begin(), path.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
  };
  if (!isValidObjectId(commit) || !printable(lock_path) || !isSafeRelativePath(lock_path))
    throw std::invalid_argument("dependency request requires an exact commit and a safe lock path");
  const auto metadata = std::filesystem::symlink_status(repository);
  if (!std::filesystem::is_directory(metadata) || std::filesystem::is_symlink(metadata))
    throw std::runtime_error("dependency project repository is unavailable");
  ProcessOptions options;
  options.timeout = std::chrono::seconds(20);
  options.output_limit = 4 * 1024 * 1024;
  options.inherit_stderr = true;
  options.environment["GIT_NO_REPLACE_OBJECTS"] = "1";
  for (const auto* name : {"GIT_DIR", "GIT_COMMON_DIR", "GIT_WORK_TREE", "GIT_OBJECT_DIRECTORY",
                           "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_REPLACE_REF_BASE", "GIT_NAMESPACE"})
    options.environment[name] = std::nullopt;
  const auto object_type =
      runProcess({"git", "--git-dir", repository.string(), "cat-file", "-t", std::string(commit)}, options);
  if (object_type.exit_code != 0 || object_type.timed_out || object_type.output_truncated ||
      object_type.output != "commit\n")
    throw std::runtime_error("dependency request object is not an exact commit");
  const auto read_blob = [&](const std::string& path) {
    const auto entry = runProcess({"git", "--literal-pathspecs", "--git-dir", repository.string(), "ls-tree",
                                   "-z", "--full-tree", std::string(commit), "--", path},
                                  options);
    const auto tab = entry.output.find('\t');
    if (entry.exit_code != 0 || entry.timed_out || entry.output_truncated || tab == std::string::npos ||
        entry.output.empty() || entry.output.back() != '\0' ||
        entry.output.find('\0') != entry.output.size() - 1 ||
        entry.output.substr(tab + 1, entry.output.size() - tab - 2) != path ||
        (!entry.output.starts_with("100644 blob ") && !entry.output.starts_with("100755 blob ")))
      throw std::runtime_error("dependency input must be a regular file in the requested commit");
    const auto result = runProcess(
        {"git", "--git-dir", repository.string(), "cat-file", "-p", std::string(commit) + ":" + path},
        options);
    if (result.exit_code != 0 || result.timed_out || result.output_truncated)
      throw std::runtime_error("cannot read bounded dependency input from the requested commit");
    return result.output;
  };
  auto bytes = read_blob(std::string(lock_path));
  auto lock = parseCiDependencyLock(bytes);
  verifyCiDependencyInputs(lock, read_blob);
  return {std::move(lock), std::move(bytes)};
}
void withReviewedCiDependencyCommit(const std::filesystem::path& state_root, std::string_view project,
                                    const std::filesystem::path& repository, std::string_view commit,
                                    std::string_view lock_path, std::string_view expected_digest,
                                    std::string_view platform, std::string_view architecture,
                                    const std::function<void(const CiDependencyCommitLock&)>& transaction) {
  if (!transaction || !isValidSha256Hex(expected_digest))
    throw std::invalid_argument("dependency admission requires a transaction and an exact lock digest");
  const auto committed = readCiDependencyCommitLock(repository, commit, lock_path);
  if (committed.lock.sha256 != expected_digest)
    throw std::runtime_error("dependency admission lock does not match the exact commit");
  withApprovedCiDependencyReview(state_root, project, expected_digest, commit, platform, architecture,
                                 [&](const CiDependencyRequest&) { transaction(committed); });
}
}  // namespace ckgit
