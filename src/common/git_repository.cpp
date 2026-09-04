// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/git_repository.hpp"

#include <algorithm>
#include <cerrno>
#include <set>
#include <sstream>
#include <stdexcept>

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

ProcessResult git(const std::filesystem::path& path,
                  std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "-C", path.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return runProcess(command);
}

std::string requireGitOutput(const std::filesystem::path& path,
                             std::initializer_list<std::string> arguments,
                             std::string_view operation) {
  const ProcessResult result = git(path, arguments);
  if (result.timed_out || result.output_truncated || result.exit_code != 0) {
    throw std::runtime_error("Git could not " + std::string(operation) + " for " +
                             path.string());
  }
  return trimTrailingNewlines(result.output);
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
      } else if (entry.path().filename() == ".git" &&
                 (std::filesystem::is_directory(status) ||
                  std::filesystem::is_regular_file(status))) {
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
  } else if (branch.exit_code == 1) {
    audit.detached_head = true;
  } else {
    throw std::runtime_error("Git could not inspect HEAD for " + top_level.string());
  }

  const auto porcelain = requireGitOutput(
      top_level, {"status", "--porcelain=v1", "--untracked-files=normal"}, "read worktree status");
  audit.changed_entries = splitLines(porcelain).size();

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
                               top_level.string());
    }
    for (const auto& url : splitLines(urls.output)) {
      audit.remotes.push_back(parseRemoteUrl(remote_name, url));
    }
  }
  return audit;
}

}  // namespace ckgit
