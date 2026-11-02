// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "tool_environment.hpp"
#include <set>
#include <stdexcept>
#include <string_view>

namespace ckgit {
std::vector<std::string> vmGuestToolEnvironment(char* const* environment) {
  constexpr std::string_view prefix = "CKGIT_VM_ENV_";
  std::vector<std::string> result;
  std::set<std::string> seen;
  if (!environment) return result;
  for (auto item = environment; *item; ++item) {
    const std::string_view entry(*item);
    if (!entry.starts_with(prefix)) continue;
    const auto value = entry.substr(prefix.size());
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) throw std::invalid_argument("invalid guest tool environment");
    const auto name = value.substr(0, equals);
    const auto content = value.substr(equals + 1);
    if (name.empty() || name.size() > 128 || name.front() < 'A' || name.front() > 'Z' ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string_view::npos ||
        content.empty() || content.size() > 4096 || content.find_first_of("\r\n") != std::string_view::npos ||
        name == "PATH" || name == "HOME" || name == "TMPDIR" || name == "SHELL" || name == "CI" ||
        name == "LANG" || name == "ENV" || name == "BASH_ENV" || name == "CDPATH" || name == "IFS" ||
        name.starts_with("CKGIT_") || name.starts_with("DYLD_") || name.starts_with("LD_") ||
        name.starts_with("GIT_") || name.starts_with("SSH_") || name.starts_with("LC_") ||
        !seen.insert(std::string(name)).second || result.size() >= 32)
      throw std::invalid_argument("invalid or reserved guest tool environment");
    result.emplace_back(value);
  }
  return result;
}
void validateVmGuestToolEnvironment(const CiWorkflow& workflow, const std::vector<std::string>& environment) {
  for (const auto& assignment : environment) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos) throw std::invalid_argument("invalid guest tool binding");
    const auto name = assignment.substr(0, equals);
    for (const auto& cache : workflow.caches)
      for (const auto& variable : cache.env)
        if (variable == name) throw std::invalid_argument("workflow cache shadows guest tool environment");
  }
}
}
