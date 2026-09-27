// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_store.hpp"

namespace ckgit {

// The CI control responses behind `ckgit ci` (docs/protocol/01-ssh-and-control-v1.md):
// the daemon formats them from the CI store and the client parses them back,
// so both ends share one definition of every line. Every response is bounded
// printable ASCII; free text (refs, details, step names, notes, log bytes)
// travels hex-encoded, and each timestamp is judged against the server clock
// the response carries rather than the client's.

// One run as listed by `ci-status` and `ci-overview`: the record's header
// fields only (`run` carries no detail, steps, or artifacts) plus the number of
// completed steps, which is also the running step's index.
struct CiRunSummary {
  CiRunRecord run;
  std::size_t completed_steps = 0;
};

struct CiProjectStatus {
  std::string project_name;
  bool ci_enabled = false;
  std::vector<CiRunSummary> runs;  // newest first
};

// `ci-status` reports one project with its kMaximumControlCiRuns newest runs;
// `ci-overview` reports every hosted project with its newest run and, when that
// run is not active, the newest active one (see kControlCiOverviewScanRuns).
struct CiStatusReport {
  std::uint64_t server_epoch_seconds = 0;
  std::vector<CiProjectStatus> projects;
};

// `ci-run`: one run with its detail, completed steps, and artifacts.
struct CiRunReport {
  std::uint64_t server_epoch_seconds = 0;
  CiRunRecord run;
};

// `ci-log`: up to kMaximumControlLogChunkBytes of one step's log starting at
// `offset`. `complete` means the step's log has ended and this chunk reaches
// its last byte; otherwise the caller asks again from offset + bytes.size().
struct CiLogChunk {
  std::uint64_t offset = 0;
  std::string bytes;
  bool complete = false;
};

// Strips a record down to the fields a summary line carries.
CiRunSummary summarizeCiRun(const CiRunRecord& run);

// Formatters throw std::length_error when the response would exceed
// kMaximumControlResponseBytes, and std::invalid_argument for a record that
// cannot be represented (an invalid id, name, commit, or oversized text).
// Parsers throw std::invalid_argument for any framing, count, order, or field
// that does not match the documented grammar.
std::string formatCiStatusControlResponse(const CiStatusReport& report);
CiStatusReport parseCiStatusControlResponse(std::string_view response);

std::string formatCiRunControlResponse(const CiRunReport& report);
// `project_name` is the project the request named; it fills run.project_name.
CiRunReport parseCiRunControlResponse(std::string_view response, std::string_view project_name);

std::string formatCiLogControlResponse(const CiLogChunk& chunk);
// Rejects a response for any other offset than the one requested.
CiLogChunk parseCiLogControlResponse(std::string_view response, std::uint64_t requested_offset);

}  // namespace ckgit
