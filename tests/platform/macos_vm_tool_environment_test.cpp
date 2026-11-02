// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "../../src/ci-vm-guest-macos/tool_environment.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<std::string> parse(std::vector<std::string> values) {
  std::vector<char*> entries;
  for (auto& value : values) entries.push_back(value.data());
  entries.push_back(nullptr);
  return ckgit::vmGuestToolEnvironment(entries.data());
}
bool rejects(std::vector<std::string> values) {
  try { (void)parse(std::move(values)); return false; } catch (const std::invalid_argument&) { return true; }
}
}
int main() {
  const auto result = parse({"UNRELATED=ignored", "CKGIT_VM_ENV_ORTHOMAP_OPENMVS_BIN=/opt/OpenMVS/bin"});
  if (result != std::vector<std::string>{"ORTHOMAP_OPENMVS_BIN=/opt/OpenMVS/bin"} ||
      !rejects({"CKGIT_VM_ENV_PATH=/tmp"}) || !rejects({"CKGIT_VM_ENV_DYLD_INSERT_LIBRARIES=/bad"}) ||
      !rejects({"CKGIT_VM_ENV_CKGIT_CI=0"}) || !rejects({"CKGIT_VM_ENV_1BAD=x"}) ||
      !rejects({"CKGIT_VM_ENV_TOOL=x\ny"}) || !rejects({"CKGIT_VM_ENV_TOOL="}) ||
      !rejects({"CKGIT_VM_ENV_TOOL=a", "CKGIT_VM_ENV_TOOL=b"})) return 1;
  ckgit::CiWorkflow workflow;
  workflow.caches.push_back({"cache", {"ORTHOMAP_OPENMVS_BIN"}});
  bool collision = false;
  try { ckgit::validateVmGuestToolEnvironment(workflow, result); }
  catch (const std::invalid_argument&) { collision = true; }
  if (!collision) return 1;
  std::cout << "VM guest tool environment passed\n";
  return 0;
}
