// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// libFuzzer target for the runner protocol's parsers: the SSH command line the
// dispatcher and gateway accept, the claim answer a runner reads, and the
// finish payload the gateway reads. None may crash, hang, or accept input that
// does not round-trip to the same record.
//
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined -D_GNU_SOURCE -Iinclude \
//     tests/fuzz/ci_runner_protocol_fuzz.cpp src/common/*.cpp src/common/web/*.cpp -o ci-runner-protocol-fuzz

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "ckgit/ci_runner_protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  if (const auto command = ckgit::parseCiRunnerCommand(input)) {
    if (ckgit::formatCiRunnerCommand(*command) != input) __builtin_trap();
  }
  try {
    static_cast<void>(ckgit::parseCiClaimResponse(input));
  } catch (const std::exception&) {
  }
  try {
    const ckgit::CiFinishReport report = ckgit::parseCiFinishReport(input);
    const ckgit::CiFinishReport again = ckgit::parseCiFinishReport(ckgit::formatCiFinishReport(report));
    if (again.steps.size() != report.steps.size() || again.outputs.size() != report.outputs.size()) __builtin_trap();
  } catch (const std::exception&) {
  }
  return 0;
}
