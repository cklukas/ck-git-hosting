// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_workflow.hpp"
#include "ckgit/yaml_subset.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

// The syntax layer alone, under the workflow's bounds: arbitrary bytes are
// almost always outside the subset, so rejection (std::runtime_error) and an
// exceeded bound (std::length_error) are both expected. The fuzzer exists to
// catch memory errors, UB, and hangs in the parser itself, independent of any
// schema on top of it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    ckgit::parseYamlSubset(std::string_view(reinterpret_cast<const char*>(data), size),
                           ckgit::kCiWorkflowYamlDialect);
  } catch (const std::exception&) {
  }
  return 0;
}
