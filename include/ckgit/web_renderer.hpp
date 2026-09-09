// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ckgit/project_summary.hpp"

namespace ckgit {

struct PageContext {
  // Named identity is kept independently from the immutable commit used to
  // read this page. A commit ID must never be relabelled as an arbitrary ref.
  std::string ref;
  std::string commit_id;
  std::string section;
  std::string path;
  int year{0};
  int month{0};
};

std::string htmlEscape(std::string_view value);
std::string renderProjectTable(const std::vector<ProjectSummary>& projects, bool sort_by_name = false);
std::string renderProjectDetail(const ProjectSummary& project, const PageContext* context = nullptr);

}  // namespace ckgit

namespace ckgit {
std::string pageLayout(std::string_view title, std::string_view body, const ProjectSummary* project = nullptr,
                       const PageContext* context = nullptr);
std::string formatUtcTimestamp(std::uint64_t epoch);
std::string relativeTime(std::uint64_t epoch);
std::string formatBytes(std::uint64_t bytes);
std::string escapePre(std::string_view value);
std::string sourceUrl(const std::string& project, const std::string& kind,
                      const std::string& ref, const std::string& path = {});
std::string refPicker(const ProjectSummary& project, const std::string& selected,
                      const std::string& kind, const std::string& path = {}, int year = 0, int month = 0);
std::string refLabel(const std::string& ref);
std::string renderReadme(const ProjectSummary& project, const std::string& id,
                         const std::string& path, const std::string& content, const std::string& ref = {});
}
