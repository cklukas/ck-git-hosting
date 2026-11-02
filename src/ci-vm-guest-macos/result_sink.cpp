// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "result_sink.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/temp_directory.hpp"
#include <array>
#include <fstream>
#include <stdexcept>

namespace ckgit {
namespace {
constexpr auto kTimeout = std::chrono::seconds(30);
}
CiVmGuestSink::CiVmGuestSink(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                             std::filesystem::path relay_root)
    : channel_(channel), assignment_(assignment), policy_(policy), relay_root_(std::move(relay_root)),
      artifacts_(assignment.workflow.jobs.size(), false) {
  if (!relay_root_.is_absolute()) throw std::invalid_argument("VM relay root must be absolute");
}
void CiVmGuestSink::acknowledge() {
  const auto reply = channel_.receive(kTimeout);
  if (reply.type == CiVmFrameType::Cancel && reply.payload.empty()) cancelled_ = true;
  else if (reply.type == CiVmFrameType::Error && reply.payload == "lease-lost") lease_lost_ = true;
  else if (reply.type != CiVmFrameType::Event || reply.payload != "ok") {
    channel_.invalidate();
    throw std::runtime_error("invalid VM host acknowledgement");
  }
}
void CiVmGuestSink::request(CiVmFrameType type, std::string_view payload) {
  channel_.send(type, payload, kTimeout);
  acknowledge();
}
void CiVmGuestSink::runStarted(const CiRunRecord& record) {
  request(CiVmFrameType::Event, "started " + std::to_string(record.started_epoch_seconds));
}
void CiVmGuestSink::stepStarted(const CiRunRecord&, std::size_t index) {
  request(CiVmFrameType::Event, "step-start " + std::to_string(index));
}
void CiVmGuestSink::appendLog(std::size_t index, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = std::min(bytes.size(), kCiVmChunkPayloadMax);
    channel_.send(CiVmFrameType::Event, "log " + std::to_string(index), kTimeout);
    channel_.send(CiVmFrameType::Chunk, bytes.substr(0, count), kTimeout);
    acknowledge();
    bytes.remove_prefix(count);
  }
}
void CiVmGuestSink::stepFinished(const CiRunRecord& record, std::size_t index) {
  if (record.steps.size() != index + 1) throw std::invalid_argument("invalid VM completed step");
  const auto& step = record.steps.back();
  request(CiVmFrameType::Event, "step-end " + std::to_string(index) + " " + std::to_string(step.exit_code) +
      " " + (step.timed_out ? "1" : "0") + " " + (step.output_truncated ? "1" : "0") + " " + hexOrDash(step.name));
}
void CiVmGuestSink::heartbeat(const CiRunRecord&) { request(CiVmFrameType::Event, "heartbeat"); }
void CiVmGuestSink::tick() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_poll_ >= std::chrono::seconds(1)) { request(CiVmFrameType::Event, "poll"); last_poll_ = now; }
}
void CiVmGuestSink::progress(const CiRunRecord& record) {
  request(CiVmFrameType::Event, "progress " + hexOrDash(record.detail));
}
void CiVmGuestSink::upload(std::string_view header, const std::filesystem::path& file, std::uint64_t size) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read VM output bundle");
  channel_.send(CiVmFrameType::Event, header, kTimeout);
  std::array<char, kCiVmChunkPayloadMax> buffer{};
  std::uint64_t remaining = size;
  while (remaining) {
    const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), count);
    if (input.gcount() != count) throw std::runtime_error("incomplete VM output bundle");
    channel_.send(CiVmFrameType::Chunk, std::string_view(buffer.data(), static_cast<std::size_t>(count)), kTimeout);
    remaining -= static_cast<std::uint64_t>(count);
  }
  if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("VM output bundle changed during upload");
  channel_.send(CiVmFrameType::Done, {}, kTimeout);
  acknowledge();
}
CiArtifactRecord CiVmGuestSink::putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                                           const std::string& note, bool release) {
  if (release != assignment_.is_release) throw std::invalid_argument("invalid VM artifact release mode");
  std::size_t index = 0;
  for (; index < artifacts_.size(); ++index) {
    const auto& declared = assignment_.workflow.jobs[index].artifact;
    if (!artifacts_[index] && declared && declared->name == artifact.name &&
        declared->paths == artifact.paths && declared->retention_days == artifact.retention_days) break;
  }
  if (index == artifacts_.size()) throw std::invalid_argument("undeclared VM artifact");
  artifacts_[index] = true;
  CiArtifactRecord result;
  result.name = artifact.name;
  result.note = note;
  const std::uint64_t size = note.empty() ? std::filesystem::file_size(bundle) : 0;
  if (size > policy_.artifact_max_bytes) throw std::runtime_error("VM artifact exceeds limit");
  const std::string header = "artifact " + std::to_string(index) + " " + std::to_string(size) + " " + hexOrDash(note);
  if (note.empty()) { upload(header, bundle, size); result.bytes = size; }
  else request(CiVmFrameType::Event, header);
  outputs_.push_back({artifact.name, release, artifact.retention_days, note});
  return result;
}
void CiVmGuestSink::putPagesSite(const std::filesystem::path& site) {
  if (!assignment_.is_default_branch || assignment_.is_release || !assignment_.workflow.pages_path)
    throw std::invalid_argument("VM Pages output not permitted");
  const auto dir = createTempDirectory(relay_root_, "vm-pages-");
  const auto archive = dir / "site.tar";
  try {
    TarPackOptions options;
    options.max_bytes = kMaximumPagesSiteBytes;
    options.max_entries = kMaximumPagesEntries + 1;
    options.timeout = std::chrono::seconds(600);
    options.skip_non_regular = true;
    options.should_continue = [this] { tick(); return !cancelled_ && !lease_lost_; };
    packRegularTreeTar(site, {"."}, archive, options);
    const auto size = std::filesystem::file_size(archive);
    upload("pages " + std::to_string(size), archive, size);
    std::filesystem::remove(archive);
    std::filesystem::remove(dir);
  } catch (...) {
    std::error_code error;
    std::filesystem::remove(archive, error);
    std::filesystem::remove(dir, error);
    throw;
  }
}
CiRunRecord CiVmGuestSink::finish(CiRunRecord record) {
  CiFinishReport report;
  report.status = record.status;
  report.started_epoch_seconds = record.started_epoch_seconds;
  report.detail = record.detail;
  report.steps = record.steps;
  report.outputs = outputs_;
  report.execution = record.execution;
  request(CiVmFrameType::Done, formatCiFinishReport(report));
  return record;
}
}
