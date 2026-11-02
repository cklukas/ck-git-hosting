// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {
// A parsed, content-addressed dependency lock. Parsing is not approval and
// this type does not authorize provisioning or CI execution.
struct CiDependencyInput {
  std::string path;
  std::string sha256;
};
struct CiDependencyPackage {
  std::string name;
  std::string version;
  std::string registry;
  std::string sha256;
  std::string recipe_sha256;
  bool direct = false;
  std::vector<std::string> needs;
};
struct CiDependencyLock {
  std::string sha256;
  std::string platform;
  std::string architecture;
  std::string abi;
  std::vector<CiDependencyInput> inputs;
  std::vector<CiDependencyPackage> packages;
};
// Strict, bounded parser. The digest is over the exact original bytes, not a
// reserialization, so every change requires a distinct approval.
CiDependencyLock parseCiDependencyLock(std::string_view content);
// Read each path from the exact commit to be built. The caller must bound blob
// reads before returning them. Missing or changed inputs must fail rather than
// reusing an approval for a different source tree.
void verifyCiDependencyInputs(const CiDependencyLock& lock,
                              const std::function<std::string(const std::string&)>& read_input);
}  // namespace ckgit
