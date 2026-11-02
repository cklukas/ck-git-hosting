// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
#include "ckgit/ci_workflow.hpp"

namespace ckgit {
// Reads only explicitly installed guest-tool bindings. These are visible to
// every job and must never contain credentials. Reserved execution variables
// cannot be supplied through this interface.
std::vector<std::string> vmGuestToolEnvironment(char* const* environment);
// Workflow cache bindings are applied after ordinary extra environment by
// the shared executor. Refuse any collision rather than letting a workflow
// replace an operator-selected tool path.
void validateVmGuestToolEnvironment(const CiWorkflow& workflow, const std::vector<std::string>& environment);
}
