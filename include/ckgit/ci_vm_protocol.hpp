// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_runner_protocol.hpp"

namespace ckgit {
// Framing only. The receiver must separately validate message direction,
// ordering, identifiers, checkout scope and cumulative log/artifact limits.
enum class CiVmFrameType : std::uint8_t {
  Hello = 1, Assignment = 2, Request = 3, Event = 4,
  Chunk = 5, Done = 6, Cancel = 7, Error = 8
};
inline constexpr std::size_t kCiVmControlPayloadMax = 256 * 1024;
inline constexpr std::size_t kCiVmChunkPayloadMax = 64 * 1024;
inline constexpr std::size_t kCiVmInputBufferMax = 512 * 1024;
struct CiVmFrame {
  CiVmFrameType type;
  std::string payload;
};
std::string encodeCiVmFrame(CiVmFrameType type, std::string_view payload);

// A guest assignment deliberately has no runner credentials or usable lease
// token. Network access is forced off regardless of the host policy.
struct CiVmAssignment {
  CiPreparedJob prepared;
  CiRemotePolicy policy;
};
std::string formatCiVmAssignment(const CiPreparedJob& prepared, const CiRemotePolicy& policy);
CiVmAssignment parseCiVmAssignment(std::string_view payload);

// The host must validate every guest checkout against its own original
// assignment before accessing the server. No guest-provided destination path
// participates in this decision.
bool ciVmCheckoutAllowed(const CiPreparedJob& assignment, const CiCheckoutRequest& request);

// Checkout requests name only an ordinal: zero is the primary checkout and
// subsequent ordinals select a resolved sister in the host's assignment. The
// host never accepts project names, refs, paths, or history settings from the
// guest as authority for a checkout.
std::string formatCiVmCheckoutRequest(std::size_t ordinal);
CiCheckoutRequest parseCiVmCheckoutRequest(std::string_view payload, const CiPreparedJob& assignment);

// Host-owned accounting for untrusted guest output. Step and artifact indices
// are resolved against the host assignment, not names or paths from the guest.
// Artifact limits apply to each bundle, matching the existing CI runner.
class CiVmOutputGuard {
 public:
  CiVmOutputGuard(const CiPreparedJob& assignment, const CiRemotePolicy& policy);
  void log(std::size_t step, std::uint64_t bytes);
  const CiArtifact& beginArtifact(std::size_t job, std::uint64_t declared_bytes);
  void artifactChunk(std::uint64_t bytes);
  void endArtifact();
  bool artifactOpen() const { return artifact_open_; }

 private:
  const CiPreparedJob& assignment_;
  std::uint64_t max_log_;
  std::uint64_t max_artifact_;
  std::vector<std::uint64_t> logs_;
  std::vector<bool> artifacts_;
  std::uint64_t artifact_remaining_ = 0;
  bool artifact_open_ = false;
};

// Feed bounded reads and drain complete frames after each feed. A malformed or
// oversized frame permanently poisons the decoder; close the connection.
class CiVmFrameDecoder {
 public:
  void feed(std::string_view bytes);
  std::optional<CiVmFrame> next();
  // Call at EOF after draining completed frames; a partial frame is invalid.
  void finish();
  bool failed() const { return failed_; }
  std::size_t bufferedBytes() const { return input_.size(); }

 private:
  std::string input_;
  bool failed_ = false;
};

#ifndef _WIN32
// A single-caller, non-owning socket channel. Timeouts, cancellation, malformed
// input and disconnects poison the channel; its owner must close the socket.
// A caller still enforces the message state machine and cumulative budgets.
class CiVmChannel {
 public:
  explicit CiVmChannel(int socket_fd);
  void send(CiVmFrameType type, std::string_view payload, std::chrono::milliseconds timeout,
            const std::function<bool()>& should_continue = {});
  CiVmFrame receive(std::chrono::milliseconds timeout, const std::function<bool()>& should_continue = {});
  bool failed() const { return failed_; }
  void invalidate() { failed_ = true; }

 private:
  int fd_;
  bool failed_ = false;
  CiVmFrameDecoder decoder_;
};
// Both sides must close the channel after any handshake failure. The host
// sends only a sanitized assignment after the exact guest protocol greeting.
void ciVmHostHandshake(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                       std::chrono::milliseconds timeout, const std::function<bool()>& should_continue = {});
CiVmAssignment ciVmGuestHandshake(CiVmChannel& channel, std::chrono::milliseconds timeout,
                                   const std::function<bool()>& should_continue = {});
#endif
}
