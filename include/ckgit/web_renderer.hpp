// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ckgit/project_summary.hpp"

namespace ckgit {

std::string htmlEscape(std::string_view value);
std::string renderProjectTable(const std::vector<ProjectSummary>& projects);
std::string renderProjectDetail(const ProjectSummary& project);

}  // namespace ckgit
