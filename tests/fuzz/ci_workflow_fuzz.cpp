// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_workflow.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

// Arbitrary bytes are almost always outside the supported subset, so rejection
// (std::runtime_error) and exceeding a documented bound (std::length_error) are
// both expected. The fuzzer exists to catch memory errors, UB, and hangs — not
// to assert that malformed input parses.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    ckgit::parseCiWorkflow(std::string_view(reinterpret_cast<const char*>(data), size));
  } catch (const std::exception&) {
  }
  return 0;
}
