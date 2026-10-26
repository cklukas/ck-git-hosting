// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/process.hpp"
namespace ckgit {
std::string selectedProcessProgram(const std::string& program);
std::vector<std::string> selectedProcessArguments(const std::vector<std::string>& arguments);
std::optional<ProcessOptions> selectedProcessOptions(const std::vector<std::string>& arguments, const ProcessOptions& options);
}
