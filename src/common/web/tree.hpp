// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/project_summary.hpp"
#include "ckgit/web_repository.hpp"
namespace ckgit {
std::string renderFileTree(WebRepository& repository, const ProjectSummary& project,
                          const std::string& commit, const std::string& ref,
                          const std::string& directory, const std::string& selected);
}
