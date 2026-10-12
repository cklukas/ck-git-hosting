// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"

namespace ckgit {

// The runner protocol, version 1: how a remote runner works on the main
// host's queue through the restricted SSH account. A runner key's forced
// command accepts only
//
//   ckgit-runner 1 hello <version-hex>
//   ckgit-runner 1 claim <wait-seconds>
//   ckgit-runner 1 upload-pack <job-id> <token> <project>
//   ckgit-runner 1 stream <job-id> <token>
//   ckgit-runner 1 heartbeat <job-id> <token>
//   ckgit-runner 1 put <job-id> <token> <kind> <name> <offset> <total> <sha256>
//   ckgit-runner 1 finish <job-id> <token>
//   ckgit-runner 1 abandon <job-id> <token>
//
// with unquoted tokens of [A-Za-z0-9._-] only, and runs them in the gateway
// (`ck-ci-runnerd gateway`) on the main host. Payloads -- a put's chunk, a
// finish's final record, a stream's frames, upload-pack's pack protocol -- use
// the SSH session's stdin and stdout. Every operation on a job carries the
// lease token its claim returned; a request with any other token is refused,
// so a runner that lost its job can never write into the next attempt.

inline constexpr int kCiRunnerProtocolVersion = 1;
inline constexpr unsigned kMaximumCiClaimWaitSeconds = 120;
inline constexpr std::size_t kMaximumCiFinishPayloadBytes = 512 * 1024;
inline constexpr std::size_t kMaximumCiClaimResponseBytes = 256 * 1024;
// A stream's log frame carries at most this many bytes.
inline constexpr std::size_t kMaximumCiLogFrameBytes = 64 * 1024;

enum class CiRunnerOp { kHello, kClaim, kUploadPack, kStream, kHeartbeat, kPut, kFinish, kAbandon };

struct CiRunnerCommand {
  CiRunnerOp op = CiRunnerOp::kHello;
  std::string version_hex;  // hello
  unsigned wait_seconds = 0;  // claim
  std::string job_id;
  std::string token;
  std::string project;  // upload-pack
  std::string kind;     // put: artifact, asset, or pages
  std::string name;     // put
  std::uint64_t offset = 0;
  std::uint64_t total = 0;
  std::string sha256;
};

// Parses a complete `ckgit-runner 1 ...` command line; std::nullopt (with a
// safe `reason`) for anything else.
std::optional<CiRunnerCommand> parseCiRunnerCommand(std::string_view command, std::string* reason = nullptr);
// The command line for `command`, as the runner sends it.
std::string formatCiRunnerCommand(const CiRunnerCommand& command);

// The policy a runner must apply, sent with every claimed job: the main host's
// limits are authoritative (a runner may only tighten them).
struct CiRemotePolicy {
  unsigned timeout_seconds = 1800;
  std::uint64_t max_log_bytes = 1u << 20;
  std::uint64_t artifact_max_bytes = static_cast<std::uint64_t>(256) << 20;
  bool allow_network = false;
  unsigned lease_seconds = kCiDefaultLeaseSeconds;
};

// A claimed job as the runner receives it.
struct CiRemoteJob {
  CiPreparedJob prepared;
  std::string token;
  CiRemotePolicy policy;
};

// `claim`'s answer: "none\n", or "job\n" followed by the prepared job.
std::string formatCiClaimResponse(const CiPreparedJob& prepared, const CiLease& lease, const CiRemotePolicy& policy);
// std::nullopt for "none"; throws std::invalid_argument for a malformed answer.
std::optional<CiRemoteJob> parseCiClaimResponse(std::string_view response);

// `finish`'s payload: the final record and the outputs the run declared.
struct CiFinishReport {
  CiRunStatus status = CiRunStatus::Error;
  std::uint64_t started_epoch_seconds = 0;
  std::string detail;
  std::vector<CiStepResult> steps;
  std::vector<CiDeclaredOutput> outputs;
};
std::string formatCiFinishReport(const CiFinishReport& report);
CiFinishReport parseCiFinishReport(std::string_view payload);

// --- stream frames ------------------------------------------------------------
//
// Runner to main host, one line each (a log line is followed by exactly
// <length> raw bytes):
//   started <epoch>
//   step-start <n> <name-hex>
//   log <n> <offset> <length>
//   step-end <n> <exit> <timed-out 0|1> <truncated 0|1> <name-hex>
//   heartbeat
//   progress <detail-hex>
// Main host to runner:
//   held <n> <bytes>      (at stream start, for every step log it holds)
//   ready
//   ack <n> <bytes>       (log bytes of step n held durably)
//   cancel
//   lease-lost
// An empty hex field is written as "-".

std::string hexOrDash(std::string_view text);
std::optional<std::string> fromHexOrDash(std::string_view token, std::size_t cap);

}  // namespace ckgit
