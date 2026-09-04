// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/project_summary.hpp"

#include <algorithm>
#include <stdexcept>

#include "ckgit/process.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

std::string trimNewlines(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

}  // namespace

std::vector<ProjectSummary> inspectHostedProjects(
    const std::filesystem::path& requested_root,
    const std::optional<std::filesystem::path>& metadata_root) {
  const auto root = validatedRepositoryRoot(requested_root);
  std::error_code error;
  std::filesystem::directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied,
                                               error);
  if (error) {
    throw std::runtime_error("cannot inspect hosted project root");
  }
  std::vector<ProjectSummary> projects;
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto entry = *iterator;
    const auto status = entry.symlink_status(error);
    if (!error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status)) {
      const std::string filename = entry.path().filename().string();
      constexpr std::string_view suffix{".git"};
      if (filename.size() > suffix.size() &&
          std::string_view(filename).substr(filename.size() - suffix.size()) == suffix) {
        const std::string name = filename.substr(0, filename.size() - suffix.size());
        if (isValidProjectName(name)) {
          const auto refs = runProcess({"git", "--git-dir", entry.path().string(), "for-each-ref",
                                        "--format=%(refname)", "refs/heads", "refs/tags"});
          if (refs.exit_code == 0 && !refs.timed_out && !refs.output_truncated) {
            const auto head = runProcess(
                {"git", "--git-dir", entry.path().string(), "symbolic-ref", "--quiet", "--short", "HEAD"});
            ProjectSummary summary;
            summary.name = name;
            summary.default_branch = head.exit_code == 0 ? trimNewlines(head.output) : "";
            for (std::size_t start = 0; start < refs.output.size();) {
              const std::size_t newline = refs.output.find('\n', start);
              const std::string_view ref(refs.output.data() + start,
                  (newline == std::string::npos ? refs.output.size() : newline) - start);
              if (ref.rfind("refs/heads/", 0) == 0) {
                ++summary.branch_count;
              } else if (ref.rfind("refs/tags/", 0) == 0) {
                ++summary.tag_count;
              }
              if (newline == std::string::npos) {
                break;
              }
              start = newline + 1;
            }
            summary.valid_head = false;
            // A symbolic HEAD is valid only when its branch actually exists.
            if (!summary.default_branch.empty()) {
              const auto head_ref = "refs/heads/" + summary.default_branch;
              const auto verification = runProcess({"git", "--git-dir", entry.path().string(), "show-ref",
                                                    "--verify", "--quiet", head_ref});
              summary.valid_head = verification.exit_code == 0;
            }
            if (metadata_root.has_value()) {
              summary.checkouts = loadCheckoutMetadata(*metadata_root, name);
              summary.events = loadProjectEvents(*metadata_root, name);
            }
            projects.push_back(std::move(summary));
          }
        }
      }
    }
    error.clear();
    iterator.increment(error);
    if (error) {
      error.clear();
    }
  }
  std::sort(projects.begin(), projects.end(), [](const ProjectSummary& left, const ProjectSummary& right) {
    return left.name < right.name;
  });
  return projects;
}

}  // namespace ckgit
