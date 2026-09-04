// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

namespace ckgit {

// First-release repository names are deliberately a narrow, portable subset.
// The returned string is empty when the value is valid.
std::string projectNameError(std::string_view name);

bool isValidProjectName(std::string_view name);
bool isValidClientId(std::string_view client_id);
bool isValidBranchName(std::string_view branch);

}  // namespace ckgit
