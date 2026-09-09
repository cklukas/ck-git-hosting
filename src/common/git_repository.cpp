// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/git_repository.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

#include "ckgit/process.hpp"

namespace ckgit {
namespace {

std::string trimTrailingNewlines(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> splitLines(const std::string& value) {
  std::vector<std::string> lines;
  std::istringstream stream(value);
  for (std::string line; std::getline(stream, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

// Inspection is read-only but not free: status on a large working tree or a
// cold disk can take well over the process helper's short default.
constexpr std::chrono::seconds kInspectionTimeout{120};
constexpr std::size_t kStatusOutputLimit = 8 * 1024 * 1024;

constexpr std::size_t kListingOutputLimit = 8 * 1024 * 1024;

ProcessResult git(const std::filesystem::path& path,
                  std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "-C", path.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return runProcess(command, kInspectionTimeout, kListingOutputLimit);
}

// Says which of the three failure modes happened, with Git's own last line.
std::string describeGitFailure(const ProcessResult& result) {
  if (result.timed_out) {
    return "timed out after " + std::to_string(kInspectionTimeout.count()) + " s";
  }
  if (result.output_truncated) {
    return "output exceeded " + std::to_string(kListingOutputLimit / (1024 * 1024)) + " MiB";
  }
  std::string tail;
  for (const auto& line : splitLines(result.output)) {
    tail = line;
  }
  for (auto& character : tail) {
    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) {
      character = ' ';
    }
  }
  if (tail.size() > 200) {
    tail.resize(200);
  }
  return "exit code " + std::to_string(result.exit_code) + (tail.empty() ? "" : ": " + tail);
}

std::string requireGitOutput(const std::filesystem::path& path,
                             std::initializer_list<std::string> arguments,
                             std::string_view operation) {
  const ProcessResult result = git(path, arguments);
  if (result.timed_out || result.output_truncated || result.exit_code != 0) {
    throw std::runtime_error("Git could not " + std::string(operation) + " for " +
                             path.string() + " (" + describeGitFailure(result) + ")");
  }
  return trimTrailingNewlines(result.output);
}

// A `.git` directory without HEAD is debris Git itself ignores, for example
// the leftover socket of a file monitor after the repository was removed.
bool isRepositoryEntry(const std::filesystem::path& git_entry, const std::filesystem::file_status& status) {
  if (std::filesystem::is_regular_file(status)) {
    return true;  // A worktree or submodule gitfile.
  }
  std::error_code error;
  return std::filesystem::is_directory(status) &&
         std::filesystem::is_regular_file(std::filesystem::symlink_status(git_entry / "HEAD", error));
}

std::filesystem::path canonicalDirectory(const std::filesystem::path& path) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(path, error);
  if (error) {
    throw std::runtime_error("cannot resolve " + path.string() + ": " + error.message());
  }
  return resolved;
}

bool globMatches(std::string_view pattern, std::string_view value) {
  std::size_t pattern_index = 0;
  std::size_t value_index = 0;
  std::size_t star_index = std::string_view::npos;
  std::size_t star_value_index = 0;
  while (value_index < value.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      star_value_index = value_index;
    } else if (pattern_index < pattern.size() && pattern[pattern_index] == value[value_index]) {
      ++pattern_index;
      ++value_index;
    } else if (star_index != std::string_view::npos) {
      pattern_index = star_index + 1;
      value_index = ++star_value_index;
    } else {
      return false;
    }
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

bool isExcluded(const std::filesystem::path& entry, const std::filesystem::path& root,
                const std::vector<std::string>& exclusions) {
  std::error_code error;
  const auto relative = std::filesystem::relative(entry, root, error);
  if (error) {
    return false;
  }
  const std::string candidate = relative.generic_string();
  for (const auto& pattern : exclusions) {
    if (globMatches(pattern, candidate)) {
      return true;
    }
    // A leading */ is conventionally used to mean any directory depth,
    // including the configured scan root itself.
    if (pattern.rfind("*/", 0) == 0 && globMatches(std::string_view(pattern).substr(2), candidate)) {
      return true;
    }
  }
  return false;
}

}  // namespace

DiscoveryResult discoverWorkingTrees(const std::vector<std::filesystem::path>& roots,
                                     const std::vector<std::string>& exclusions) {
  DiscoveryResult result;
  std::set<std::filesystem::path> found;
  for (const auto& requested_root : roots) {
    std::error_code error;
    const auto root = std::filesystem::canonical(requested_root, error);
    if (error || !std::filesystem::is_directory(root, error)) {
      result.warnings.push_back("unavailable scan root: " + requested_root.string());
      continue;
    }
    if (access(root.c_str(), R_OK | X_OK) != 0) {
      // The iterator below skips permission errors silently; an unreadable
      // root must not look like a root without repositories.
      result.warnings.push_back("unreadable scan root: " + requested_root.string());
      continue;
    }

    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
      result.warnings.push_back("cannot scan root: " + root.string());
      continue;
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
      const auto entry = *iterator;
      const auto status = entry.symlink_status(error);
      if (error) {
        result.warnings.push_back("cannot inspect: " + entry.path().string());
        error.clear();
        iterator.increment(error);
        continue;
      }
      if (isExcluded(entry.path(), root, exclusions)) {
        if (std::filesystem::is_directory(status)) {
          iterator.disable_recursion_pending();
        }
      } else if (std::filesystem::is_symlink(status)) {
        if (entry.is_directory(error)) {
          iterator.disable_recursion_pending();
        }
      } else if (entry.path().filename() == ".git" && isRepositoryEntry(entry.path(), status)) {
        std::error_code parent_error;
        const auto parent = std::filesystem::canonical(entry.path().parent_path(), parent_error);
        if (parent_error) {
          result.warnings.push_back("cannot resolve repository: " +
                                    entry.path().parent_path().string());
        } else {
          found.insert(parent);
        }
        if (std::filesystem::is_directory(status)) {
          iterator.disable_recursion_pending();
        }
      }
      iterator.increment(error);
      if (error) {
        result.warnings.push_back("cannot continue scan at: " + entry.path().string());
        error.clear();
      }
    }
  }
  result.repositories.assign(found.begin(), found.end());
  return result;
}

bool hasRepositoryContext(const std::filesystem::path& requested_path) {
  auto path = canonicalDirectory(requested_path);
  if (!std::filesystem::is_directory(path)) {
    throw std::runtime_error(path.string() + " is not a directory");
  }
  for (;;) {
    if (access(path.c_str(), X_OK) != 0) {
      throw std::runtime_error("cannot inspect directory: " + path.string());
    }
    std::error_code error;
    const auto metadata = std::filesystem::symlink_status(path / ".git", error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("cannot inspect Git metadata: " + (path / ".git").string() +
                               ": " + error.message());
    }
    // symlink_status also recognizes a dangling .git symlink. An invalid
    // marker must still select normal repository inspection and its error.
    if (std::filesystem::exists(metadata)) {
      return true;
    }
    error.clear();
    const bool head = std::filesystem::exists(
        std::filesystem::symlink_status(path / "HEAD", error));
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("cannot inspect Git HEAD: " + path.string() + ": " + error.message());
    }
    if (head) {
      const bool objects = std::filesystem::is_directory(path / "objects", error);
      if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("cannot inspect Git objects: " + path.string() + ": " + error.message());
      }
      error.clear();
      const bool refs = std::filesystem::is_directory(path / "refs", error);
      if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("cannot inspect Git refs: " + path.string() + ": " + error.message());
      }
      if (objects && refs) {
        return true;
      }
    }
    const auto parent = path.parent_path();
    if (parent == path) {
      return false;
    }
    path = parent;
  }
}

DiscoveryResult discoverImmediateWorkingTrees(const std::filesystem::path& folder) {
  DiscoveryResult result;
  const auto root = canonicalDirectory(folder);
  if (!std::filesystem::is_directory(root)) {
    throw std::runtime_error(root.string() + " is not a directory");
  }
  if (access(root.c_str(), R_OK | X_OK) != 0) {
    throw std::runtime_error("unreadable scan folder: " + root.string());
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator(root, error);
  if (error) {
    throw std::runtime_error("cannot scan folder: " + root.string() + ": " + error.message());
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto child = iterator->path();
    const auto inspect_child = [&] {
      const auto status = iterator->symlink_status(error);
      if (error) {
        result.warnings.push_back("cannot inspect: " + child.string() + ": " + error.message());
        return;
      }
      if (!std::filesystem::is_directory(status)) {
        return;
      }
      if (access(child.c_str(), R_OK | X_OK) != 0) {
        result.warnings.push_back("unreadable directory: " + child.string());
        return;
      }
      const auto git_entry = child / ".git";
      const auto git_status = std::filesystem::symlink_status(git_entry, error);
      if (error && error != std::errc::no_such_file_or_directory) {
        result.warnings.push_back("cannot inspect: " + git_entry.string() + ": " + error.message());
        return;
      }
      if (!std::filesystem::exists(git_status)) {
        return;
      }
      if (std::filesystem::is_symlink(git_status)) {
        result.warnings.push_back("skipping symlinked Git metadata: " + git_entry.string());
        return;
      }
      if (!isRepositoryEntry(git_entry, git_status)) {
        result.warnings.push_back("malformed Git metadata: " + git_entry.string());
        return;
      }
      try {
        const auto repository_root = canonicalDirectory(requireGitOutput(
            child, {"rev-parse", "--show-toplevel"}, "find repository root"));
        if (repository_root != child) {
          result.warnings.push_back("not a repository root: " + child.string());
          return;
        }
        result.repositories.push_back(child);
      } catch (const std::exception& exception) {
        result.warnings.push_back("cannot inspect repository: " + child.string() + ": " + exception.what());
      }
    };
    inspect_child();
    error.clear();
    iterator.increment(error);
    if (error) {
      result.warnings.push_back("cannot continue scan after: " + child.string() + ": " + error.message());
      break;
    }
  }
  std::sort(result.repositories.begin(), result.repositories.end());
  std::sort(result.warnings.begin(), result.warnings.end());
  return result;
}

RepositoryAudit inspectRepository(const std::filesystem::path& input_path) {
  const auto path = canonicalDirectory(input_path);
  const auto inside = requireGitOutput(path, {"rev-parse", "--is-inside-work-tree"},
                                       "identify a working tree");
  if (inside != "true") {
    throw std::runtime_error(path.string() + " is not a Git working tree");
  }
  const auto top_level = canonicalDirectory(
      requireGitOutput(path, {"rev-parse", "--show-toplevel"}, "find repository root"));

  RepositoryAudit audit;
  audit.path = top_level;
  std::error_code git_entry_error;
  audit.linked_worktree = std::filesystem::is_regular_file(
      std::filesystem::symlink_status(top_level / ".git", git_entry_error));
  const ProcessResult branch = git(top_level, {"symbolic-ref", "--quiet", "--short", "HEAD"});
  if (branch.exit_code == 0 && !branch.timed_out && !branch.output_truncated) {
    audit.current_branch = trimTrailingNewlines(branch.output);
  } else if (branch.exit_code == 1 && !branch.timed_out) {
    audit.detached_head = true;
  } else {
    throw std::runtime_error("Git could not inspect HEAD for " + top_level.string() + " (" +
                             describeGitFailure(branch) + ")");
  }

  const ProcessResult porcelain = runProcess(
      {"git", "--no-optional-locks", "-C", top_level.string(), "status", "--porcelain=v1", "--untracked-files=normal"},
      kInspectionTimeout, kStatusOutputLimit);
  if (porcelain.timed_out || porcelain.exit_code != 0) {
    throw std::runtime_error("Git could not read worktree status for " + top_level.string() + " (" +
                             describeGitFailure(porcelain) + ")");
  }
  // A truncated listing still proves the tree is dirty; only the count is a lower bound.
  audit.changed_entries = splitLines(porcelain.output).size();
  audit.changed_entries_truncated = porcelain.output_truncated;

  const auto refs = requireGitOutput(
      top_level,
      {"for-each-ref", "--format=%(refname)%09%(objectname)", "refs/heads", "refs/tags"},
      "read local refs");
  for (const auto& line : splitLines(refs)) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos) {
      throw std::runtime_error("Git returned an invalid ref record for " + top_level.string());
    }
    RefTip ref{line.substr(0, separator), line.substr(separator + 1)};
    if (ref.name.rfind("refs/heads/", 0) == 0) {
      audit.branches.push_back(std::move(ref));
    } else if (ref.name.rfind("refs/tags/", 0) == 0) {
      audit.tags.push_back(std::move(ref));
    }
  }

  const auto names = requireGitOutput(top_level, {"remote"}, "read remotes");
  for (const auto& remote_name : splitLines(names)) {
    const ProcessResult urls = git(top_level, {"remote", "get-url", "--all", remote_name});
    if (urls.exit_code != 0 || urls.timed_out || urls.output_truncated) {
      throw std::runtime_error("Git could not read remote " + remote_name + " for " +
                               top_level.string() + " (" + describeGitFailure(urls) + ")");
    }
    for (const auto& url : splitLines(urls.output)) {
      audit.remotes.push_back(parseRemoteUrl(remote_name, url));
    }
  }
  return audit;
}

RepositoryAudit inspectDiscoveredRepository(const std::filesystem::path& path) {
  const auto requested = canonicalDirectory(path);
  RepositoryAudit audit = inspectRepository(requested);
  if (audit.path != requested) {
    throw std::runtime_error(requested.string() + " is not a repository root; its .git entry is stale and Git resolves it to " +
                             audit.path.string());
  }
  return audit;
}

}  // namespace ckgit
