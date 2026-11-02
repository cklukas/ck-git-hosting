// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_vm_protocol.hpp"

#include <stdexcept>
#include <utility>
#include <charconv>
#ifndef _WIN32
#include <algorithm>
#include <array>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace ckgit {
namespace {
constexpr std::size_t kHeader = 9;
bool valid(CiVmFrameType type) {
  switch (type) {
    case CiVmFrameType::Hello: case CiVmFrameType::Assignment: case CiVmFrameType::Request:
    case CiVmFrameType::Event: case CiVmFrameType::Chunk: case CiVmFrameType::Done:
    case CiVmFrameType::Cancel: case CiVmFrameType::Error: return true;
  }
  return false;
}
std::size_t limit(CiVmFrameType type) {
  return type == CiVmFrameType::Chunk ? kCiVmChunkPayloadMax : kCiVmControlPayloadMax;
}
}
std::string encodeCiVmFrame(CiVmFrameType type, std::string_view payload) {
  if (!valid(type) || payload.size() > limit(type)) throw std::invalid_argument("invalid VM frame");
  const auto n = static_cast<std::uint32_t>(payload.size());
  std::string out = "CKV1";
  out += static_cast<char>(type);
  for (int shift : {24, 16, 8, 0}) out += static_cast<char>((n >> shift) & 0xff);
  out.append(payload);
  return out;
}
std::string formatCiVmAssignment(const CiPreparedJob& prepared, const CiRemotePolicy& policy) {
  CiLease unusable;
  unusable.token = std::string(32, '0');
  auto safe_policy = policy;
  safe_policy.allow_network = false;
  safe_policy.lease_seconds = 1;  // The lease is held and renewed only by the host.
  auto payload = formatCiClaimResponse(prepared, unusable, safe_policy, 2);
  if (payload.size() > kCiVmControlPayloadMax) throw std::invalid_argument("VM assignment too large");
  return payload;
}
CiVmAssignment parseCiVmAssignment(std::string_view payload) {
  if (payload.size() > kCiVmControlPayloadMax) throw std::invalid_argument("VM assignment too large");
  auto parsed = parseCiClaimResponse(payload);
  if (!parsed || parsed->token != std::string(32, '0') || parsed->policy.allow_network ||
      parsed->policy.lease_seconds != 1 || formatCiVmAssignment(parsed->prepared, parsed->policy) != payload)
    throw std::invalid_argument("invalid or noncanonical VM assignment");
  return {std::move(parsed->prepared), parsed->policy};
}
bool ciVmCheckoutAllowed(const CiPreparedJob& assignment, const CiCheckoutRequest& request) {
  const auto matches = [&request](const std::string& project, const std::string& commit,
                                  const CiCheckout& spec, const std::string& ref) {
    return request.project == project && request.commit == commit && request.ref == ref &&
           request.spec.depth == spec.depth && request.spec.since == spec.since &&
           request.spec.tags == spec.tags && request.spec.archive == spec.archive;
  };
  if (matches(assignment.job.project_name, assignment.job.commit_id,
              assignment.workflow.checkout, assignment.job.ref)) return true;
  for (const auto& sister : assignment.sisters)
    if (matches(sister.name, sister.commit, sister.checkout, {})) return true;
  return false;
}
std::string formatCiVmCheckoutRequest(std::size_t ordinal) {
  return "checkout " + std::to_string(ordinal);
}
CiCheckoutRequest parseCiVmCheckoutRequest(std::string_view payload, const CiPreparedJob& assignment) {
  constexpr std::string_view prefix = "checkout ";
  if (!payload.starts_with(prefix)) throw std::invalid_argument("invalid VM checkout request");
  auto digits = payload.substr(prefix.size());
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
    throw std::invalid_argument("invalid VM checkout ordinal");
  std::size_t ordinal = 0;
  const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), ordinal);
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
    throw std::invalid_argument("invalid VM checkout ordinal");
  if (ordinal == 0)
    return {assignment.job.project_name, assignment.job.commit_id, assignment.workflow.checkout, assignment.job.ref};
  if (ordinal > assignment.sisters.size()) throw std::invalid_argument("VM checkout ordinal out of range");
  const auto& sister = assignment.sisters[ordinal - 1];
  return {sister.name, sister.commit, sister.checkout, {}};
}
CiVmOutputGuard::CiVmOutputGuard(const CiPreparedJob& assignment, const CiRemotePolicy& policy)
    : assignment_(assignment), max_log_(policy.max_log_bytes), max_artifact_(policy.artifact_max_bytes),
      artifacts_(assignment.workflow.jobs.size(), false) {
  if (assignment.workflow.jobs.size() > kMaximumCiJobs) throw std::invalid_argument("too many VM jobs");
  for (const auto& job : assignment.workflow.jobs) {
    if (job.steps.size() > kMaximumCiStepsPerJob) throw std::invalid_argument("too many VM steps");
    logs_.insert(logs_.end(), job.steps.size(), 0);
  }
}
void CiVmOutputGuard::log(std::size_t step, std::uint64_t bytes) {
  if (step >= logs_.size() || bytes > kCiVmChunkPayloadMax || bytes > max_log_ - logs_[step])
    throw std::invalid_argument("VM log exceeds assigned step or limit");
  logs_[step] += bytes;
}
const CiArtifact& CiVmOutputGuard::beginArtifact(std::size_t job, std::uint64_t declared_bytes) {
  if (artifact_open_ || job >= artifacts_.size() || artifacts_[job] ||
      !assignment_.workflow.jobs[job].artifact || declared_bytes > max_artifact_)
    throw std::invalid_argument("invalid or oversized VM artifact");
  artifacts_[job] = true;
  artifact_remaining_ = declared_bytes;
  artifact_open_ = true;
  return *assignment_.workflow.jobs[job].artifact;
}
void CiVmOutputGuard::artifactChunk(std::uint64_t bytes) {
  if (!artifact_open_ || bytes == 0 || bytes > kCiVmChunkPayloadMax || bytes > artifact_remaining_)
    throw std::invalid_argument("invalid VM artifact chunk");
  artifact_remaining_ -= bytes;
}
void CiVmOutputGuard::endArtifact() {
  if (!artifact_open_ || artifact_remaining_ != 0) throw std::invalid_argument("incomplete VM artifact");
  artifact_open_ = false;
}
void CiVmFrameDecoder::feed(std::string_view bytes) {
  if (failed_) throw std::invalid_argument("VM frame decoder already failed");
  if (bytes.size() > kCiVmInputBufferMax - input_.size()) {
    failed_ = true;
    input_.clear();
    throw std::invalid_argument("VM frame input buffer exceeded");
  }
  input_.append(bytes);
}
std::optional<CiVmFrame> CiVmFrameDecoder::next() {
  if (failed_) throw std::invalid_argument("VM frame decoder already failed");
  if (input_.size() < kHeader) return std::nullopt;
  const auto reject = [this]() -> void {
    failed_ = true;
    input_.clear();
    throw std::invalid_argument("malformed or oversized VM frame");
  };
  if (input_.compare(0, 4, "CKV1") != 0) reject();
  const auto type = static_cast<CiVmFrameType>(static_cast<unsigned char>(input_[4]));
  if (!valid(type)) reject();
  std::uint32_t size = 0;
  for (std::size_t i = 5; i < kHeader; ++i) size = (size << 8) | static_cast<unsigned char>(input_[i]);
  if (size > limit(type)) reject();
  if (input_.size() - kHeader < size) return std::nullopt;
  CiVmFrame frame{type, input_.substr(kHeader, size)};
  input_.erase(0, kHeader + size);
  return frame;
}
void CiVmFrameDecoder::finish() {
  if (failed_) throw std::invalid_argument("VM frame decoder already failed");
  if (!input_.empty()) {
    failed_ = true;
    input_.clear();
    throw std::invalid_argument("truncated VM frame at EOF");
  }
}
#ifndef _WIN32
namespace {
std::chrono::steady_clock::time_point deadlineFor(std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24))
    throw std::invalid_argument("invalid VM channel timeout");
  return std::chrono::steady_clock::now() + timeout;
}
void waitSocket(int fd, short events, std::chrono::steady_clock::time_point deadline,
                const std::function<bool()>& should_continue) {
  for (;;) {
    if (should_continue && !should_continue()) throw std::runtime_error("VM channel cancelled");
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("VM channel timed out");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 100));
    pollfd poller{fd, events, 0};
    const int result = ::poll(&poller, 1, timeout);
    if (result < 0 && errno == EINTR) continue;
    if (result < 0 || (result > 0 && (poller.revents & (POLLERR | POLLNVAL))))
      throw std::runtime_error("VM socket polling failed");
    // A hung-up socket can still have bytes to drain before recv reports EOF.
    if (result > 0 && (poller.revents & (events | POLLHUP))) return;
  }
}
}
CiVmChannel::CiVmChannel(int socket_fd) : fd_(socket_fd) {
  if (socket_fd < 0) throw std::invalid_argument("invalid VM socket");
}
void CiVmChannel::send(CiVmFrameType type, std::string_view payload, std::chrono::milliseconds timeout,
                       const std::function<bool()>& should_continue) {
  if (failed_) throw std::runtime_error("VM channel already failed");
  try {
    const auto frame = encodeCiVmFrame(type, payload);
    const auto deadline = deadlineFor(timeout);
    std::size_t offset = 0;
    while (offset < frame.size()) {
      waitSocket(fd_, POLLOUT, deadline, should_continue);
      const auto sent = ::send(fd_, frame.data() + offset, frame.size() - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (sent <= 0) throw std::runtime_error("VM socket write failed");
      offset += static_cast<std::size_t>(sent);
    }
  } catch (...) { failed_ = true; throw; }
}
CiVmFrame CiVmChannel::receive(std::chrono::milliseconds timeout, const std::function<bool()>& should_continue) {
  if (failed_) throw std::runtime_error("VM channel already failed");
  try {
    const auto deadline = deadlineFor(timeout);
    for (;;) {
      if (should_continue && !should_continue()) throw std::runtime_error("VM channel cancelled");
      if (auto frame = decoder_.next()) return std::move(*frame);
      waitSocket(fd_, POLLIN, deadline, should_continue);
      std::array<char, 64 * 1024> bytes{};
      const auto count = ::recv(fd_, bytes.data(), bytes.size(), MSG_DONTWAIT);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count < 0) throw std::runtime_error("VM socket read failed");
      if (count == 0) {
        decoder_.finish();
        throw std::runtime_error("VM socket closed");
      }
      decoder_.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
    }
  } catch (...) { failed_ = true; throw; }
}
void ciVmHostHandshake(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                       std::chrono::milliseconds timeout, const std::function<bool()>& should_continue) {
  try {
    const auto hello = channel.receive(timeout, should_continue);
    if (hello.type != CiVmFrameType::Hello || hello.payload != "ckgit-vm-guest-v1")
      throw std::invalid_argument("unexpected VM guest greeting");
    channel.send(CiVmFrameType::Assignment, formatCiVmAssignment(assignment, policy), timeout, should_continue);
  } catch (...) { channel.invalidate(); throw; }
}
CiVmAssignment ciVmGuestHandshake(CiVmChannel& channel, std::chrono::milliseconds timeout,
                                   const std::function<bool()>& should_continue) {
  try {
    channel.send(CiVmFrameType::Hello, "ckgit-vm-guest-v1", timeout, should_continue);
    const auto assignment = channel.receive(timeout, should_continue);
    if (assignment.type != CiVmFrameType::Assignment) throw std::invalid_argument("unexpected VM host assignment");
    return parseCiVmAssignment(assignment.payload);
  } catch (...) { channel.invalidate(); throw; }
}
#endif
}
