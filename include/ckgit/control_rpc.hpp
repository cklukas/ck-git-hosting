// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// Version-1 responses are bounded: a ref or project listing for a large
// repository must fit, while an unbounded reply can never exhaust a client.
inline constexpr std::size_t kMaximumControlResponseBytes = 256 * 1024;
inline constexpr std::size_t kMaximumControlRefs = 65536;
inline constexpr std::size_t kMaximumControlReleases = 64;
// A request line, including its "CKGIT-CONTROL/1 " prefix and final newline.
inline constexpr std::size_t kMaximumControlRequestBytes = 768;
// No operation takes more positional arguments than this (ci-log takes four).
inline constexpr std::size_t kMaximumControlArguments = 4;
// The newest runs a `ci-status` response lists for its project.
inline constexpr std::size_t kMaximumControlCiRuns = 64;
// How many of each project's newest runs `ci-overview` searches for an active
// one; the queue never holds more active runs than a project has pushes pending.
inline constexpr std::size_t kControlCiOverviewScanRuns = 16;
// The raw log bytes one `ci-log` response carries. Hex doubles them, and the
// response still fits the general bound with room for its header line.
inline constexpr std::size_t kMaximumControlLogChunkBytes = 112 * 1024;
static_assert(2 * kMaximumControlLogChunkBytes + 256 <= kMaximumControlResponseBytes);

// One version-1 control request: the client ID the SSH forced command fixed,
// the operation, and its positional arguments.
struct ControlRequest {
  std::string client_id;
  std::string operation;
  std::vector<std::string> arguments;
};

// The version-1 operation table (docs/protocol/01-ssh-and-control-v1.md),
// shared by the SSH dispatcher, the forwarding client, and the daemon so the
// three boundaries can never disagree about what is allowed. True only when
// `operation` is documented and `arguments` has exactly its arity with every
// argument valid for its position (project, branch, checkout path, CI run id,
// step index, or byte offset).
bool isValidControlOperation(std::string_view operation, const std::vector<std::string>& arguments);

// Parses one complete request line as the daemon receives it: bounded,
// printable ASCII, a single trailing newline, the "CKGIT-CONTROL/1 " prefix, a
// valid client ID, and an operation the table above accepts. std::nullopt for
// anything else.
std::optional<ControlRequest> parseControlRequest(std::string_view line);

// Sends the fixed, line-based version-1 request to the local control socket.
// It throws std::invalid_argument for a request the table above rejects,
// std::runtime_error for transport and framing failures, and returns true only
// for an `ok` response. The response is safe, bounded text for the caller.
bool forwardControlRpc(const std::filesystem::path& socket_path,
                       std::string_view client_id,
                       std::string_view operation,
                       const std::vector<std::string>& arguments,
                       std::string* response,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10));

}  // namespace ckgit
