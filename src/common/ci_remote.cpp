// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_remote.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/pages_store.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <thread>
#include "ckgit/ci_results.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/process.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

std::uint64_t parseCount(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("malformed number from the main host");
  }
  return value;
}

std::string firstLine(const std::string& text) {
  const std::size_t newline = text.find('\n');
  return newline == std::string::npos ? text : text.substr(0, newline);
}

}  // namespace

// --- remote source -------------------------------------------------------------

RemoteCiSource::RemoteCiSource(const CiRemoteTransport& transport, std::string job_id, std::string token)
    : transport_(transport), job_id_(std::move(job_id)), token_(std::move(token)) {}

void RemoteCiSource::checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination) {
  checkout(request, destination, {});
}
void RemoteCiSource::checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination,
    const std::function<bool()>& should_continue) {
  if (!isValidProjectName(request.project)) throw std::runtime_error("invalid checkout request");
  cloneCiCheckout(transport_.fetchUrl(request.project), request, destination,
                  {"GIT_SSH_COMMAND=" + transport_.gitSshCommand(job_id_, token_), "GIT_SSH_VARIANT=ssh"}, {}, should_continue);
}

// --- remote sink ---------------------------------------------------------------

RemoteCiSink::RemoteCiSink(const CiRemoteTransport& transport, const CiRemoteJob& job,
                           std::filesystem::path relay_dir, std::function<bool()> stop_requested)
    : transport_(transport), job_(job), relay_dir_(std::move(relay_dir)), stop_requested_(std::move(stop_requested)) {
  std::error_code error;
  std::filesystem::create_directories(relay_dir_, error);
  if (error) throw std::runtime_error("could not create runner relay directory");
  relay_dir_ = createTempDirectory(relay_dir_, "private-");
}

RemoteCiSink::~RemoteCiSink() {
  disconnect();
  std::error_code error;
  std::filesystem::remove_all(relay_dir_, error);
}

std::filesystem::path RemoteCiSink::relayPath(std::size_t index) const {
  return relay_dir_ / (std::to_string(index) + ".log");
}

bool RemoteCiSink::connect() {
  last_connect_attempt_ = std::chrono::steady_clock::now();
  CiRunnerCommand command;
  command.op = CiRunnerOp::kStream;
  command.job_id = job_.prepared.job.job_id;
  command.token = job_.token;
  try {
    session_ = transport_.open(command);
  } catch (const std::exception&) {
    return false;
  }
  // The greeting: what the main host already holds, then "ready".
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::string buffer;
  bool ready = false;
  while (!ready && std::chrono::steady_clock::now() < deadline) {
    char chunk[4096];
    const auto received = session_->read(chunk, sizeof(chunk));
    if (received < 0) break;
    if (!received) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    buffer.append(chunk, static_cast<std::size_t>(received));
    std::size_t newline;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (line == "ready") {
        ready = true;
        break;
      }
      if (line == "lease-lost") {
        lease_lost_ = true;
        break;
      }
      if (line.rfind("held ", 0) == 0) {
        const std::size_t space = line.find(' ', 5);
        if (space == std::string::npos) continue;
        const std::size_t index = static_cast<std::size_t>(parseCount(line.substr(5, space - 5)));
        const std::uint64_t held = parseCount(line.substr(space + 1));
        if (index < steps_.size()) {
          steps_[index].acked = std::max(steps_[index].acked, held);
          steps_[index].sent = steps_[index].acked;
        }
      }
    }
    if (lease_lost_) break;
  }
  if (!ready) {
    CiRemoteTransport::close(session_);
    return false;
  }
  reply_buffer_ = buffer;
  connected_ = true;
  resend();
  return true;
}

void RemoteCiSink::disconnect() {
  CiRemoteTransport::close(session_);
  connected_ = false;
  out_buffer_.clear();
}

// After (re)connecting, everything the main host may not hold is sent again;
// it ignores what it already has.
void RemoteCiSink::resend() {
  if (started_epoch_ != 0) send("started " + std::to_string(started_epoch_) + "\n");
  for (std::size_t index = 0; index < steps_.size(); ++index) {
    StepLog& step = steps_[index];
    step.sent = step.acked;
    send("step-start " + std::to_string(index) + " -\n");
    flushStep(index, true);
    if (step.ended) {
      send("step-end " + std::to_string(index) + " " + std::to_string(step.result.exit_code) + " " +
           (step.result.timed_out ? "1" : "0") + " " + (step.result.output_truncated ? "1" : "0") + " " +
           hexOrDash(step.result.name) + "\n");
    }
  }
  if (!detail_.empty()) send("progress " + hexOrDash(detail_) + "\n");
}

void RemoteCiSink::send(const std::string& frame) {
  if (!connected_) return;
  out_buffer_ += frame;
  pump();
}

// Writes as much of the outgoing buffer as the connection takes now.
void RemoteCiSink::pump() {
  while (connected_ && !out_buffer_.empty()) {
    const auto written = session_->write(out_buffer_);
    if (written > 0) {
      out_buffer_.erase(0, static_cast<std::size_t>(written));
      continue;
    }
    if (written == 0) return;
    disconnect();  // the connection broke; tick() reconnects and resends
    return;
  }
}

void RemoteCiSink::flushStep(std::size_t index, bool force) {
  StepLog& step = steps_[index];
  std::ifstream relay;
  while (connected_ && step.sent < step.size && (force || out_buffer_.size() < 256 * 1024)) {
    if (!relay.is_open()) relay.open(relayPath(index), std::ios::binary);
    const std::size_t length =
        static_cast<std::size_t>(std::min<std::uint64_t>(step.size - step.sent, kMaximumCiLogFrameBytes));
    std::string bytes(length, '\0');
    relay.seekg(static_cast<std::streamoff>(step.sent));
    relay.read(bytes.data(), static_cast<std::streamsize>(length));
    bytes.resize(static_cast<std::size_t>(relay.gcount()));
    if (bytes.empty()) break;
    const std::uint64_t offset = step.sent;
    step.sent += bytes.size();
    send("log " + std::to_string(index) + " " + std::to_string(offset) + " " + std::to_string(bytes.size()) + "\n" +
         bytes);
    if (!force) break;
  }
}

void RemoteCiSink::flush(bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - last_flush_ < std::chrono::milliseconds(250)) {
    // Flush early only when enough output has piled up.
    std::uint64_t pending = 0;
    for (const StepLog& step : steps_) pending += step.size - std::min(step.size, step.sent);
    if (pending < 16 * 1024) return;
  }
  last_flush_ = now;
  for (std::size_t index = 0; index < steps_.size(); ++index) flushStep(index, force);
  pump();
}

void RemoteCiSink::handleReply(const std::string& line) {
  if (line.rfind("ack ", 0) == 0 || line.rfind("held ", 0) == 0) {
    const bool held = line[0] == 'h';
    const std::size_t start = held ? 5 : 4;
    const std::size_t space = line.find(' ', start);
    if (space == std::string::npos) return;
    const std::size_t index = static_cast<std::size_t>(parseCount(line.substr(start, space - start)));
    const std::uint64_t bytes = parseCount(line.substr(space + 1));
    if (index >= steps_.size()) return;
    StepLog& step = steps_[index];
    step.acked = std::max(step.acked, bytes);
    if (held) step.sent = std::min(step.sent, bytes);  // a gap: resend from there
  } else if (line == "cancel") {
    cancel_ = true;
  } else if (line == "lease-lost") {
    lease_lost_ = true;
  } else if (line.rfind("error", 0) == 0) {
    disconnect();
  }
}

void RemoteCiSink::readReplies() {
  if (!connected_) return;
  char chunk[4096];
  for (;;) {
    const auto received = session_->read(chunk, sizeof(chunk));
    if (received > 0) {
      reply_buffer_.append(chunk, static_cast<std::size_t>(received));
      continue;
    }
    if (received == 0) break;
    disconnect();  // the main host closed the stream
    break;
  }
  std::size_t newline;
  while ((newline = reply_buffer_.find('\n')) != std::string::npos) {
    const std::string line = reply_buffer_.substr(0, newline);
    reply_buffer_.erase(0, newline + 1);
    handleReply(line);
  }
}

void RemoteCiSink::maybeHeartbeat(bool force) {
  const auto now = std::chrono::steady_clock::now();
  const auto every = std::chrono::seconds(std::max(5u, job_.policy.lease_seconds / 6));
  if (!force && now - last_heartbeat_ < every) return;
  last_heartbeat_ = now;
  send("heartbeat\n");
}

void RemoteCiSink::tick() {
  if (!connected_ && !lease_lost_ &&
      std::chrono::steady_clock::now() - last_connect_attempt_ >= std::chrono::seconds(2)) {
    connect();
  }
  readReplies();
  flush(false);
}

void RemoteCiSink::runStarted(const CiRunRecord& record) {
  started_epoch_ = record.started_epoch_seconds;
  if (connect()) {
    // resend() already announced the start.
  }
  last_heartbeat_ = std::chrono::steady_clock::now();
}

void RemoteCiSink::stepStarted(const CiRunRecord&, std::size_t index) {
  if (steps_.size() <= index) steps_.resize(index + 1);
  std::ofstream relay(relayPath(index), std::ios::binary | std::ios::trunc);
  relay.close();
  if (!relay) throw std::runtime_error("could not create runner step log");
  steps_[index] = StepLog{};
  send("step-start " + std::to_string(index) + " -\n");
}

void RemoteCiSink::appendLog(std::size_t index, std::string_view bytes) {
  if (index >= steps_.size() || bytes.empty()) return;
  std::ofstream relay(relayPath(index), std::ios::binary | std::ios::app);
  relay.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  relay.close();
  if (!relay) throw std::runtime_error("could not append runner step log");
  steps_[index].size += bytes.size();
}

void RemoteCiSink::stepFinished(const CiRunRecord& record, std::size_t index) {
  if (transport_.supportsExecutionReport())
    send("execution " + (record.execution ? formatCiExecutionReport(*record.execution) : "-") + "\n");
  if (index >= steps_.size() || record.steps.empty()) return;
  StepLog& step = steps_[index];
  step.ended = true;
  step.result = record.steps.back();
  flush(true);
  send("step-end " + std::to_string(index) + " " + std::to_string(step.result.exit_code) + " " +
       (step.result.timed_out ? "1" : "0") + " " + (step.result.output_truncated ? "1" : "0") + " " +
       hexOrDash(step.result.name) + "\n");
}

void RemoteCiSink::heartbeat(const CiRunRecord&) {
  tick();
  maybeHeartbeat(false);
}

void RemoteCiSink::progress(const CiRunRecord& record) {
  detail_ = record.detail;
  send("progress " + hexOrDash(detail_) + "\n");
}

bool RemoteCiSink::cancelRequested() { return cancel_ || stopping(); }

bool RemoteCiSink::leaseLost() { return lease_lost_; }

void RemoteCiSink::upload(const std::string& kind, const std::string& name, const std::filesystem::path& file) {
  const std::uint64_t total = std::filesystem::file_size(file);
  CiRunnerCommand command;
  command.op = CiRunnerOp::kPut;
  command.job_id = job_.prepared.job.job_id;
  command.token = job_.token;
  command.kind = kind;
  command.name = name;
  command.total = total;
  command.sha256 = sha256HexOfFile(file);
  std::ifstream in(file, std::ios::binary);
  std::uint64_t offset = 0;
  unsigned failures = 0;
  auto last_progress = std::chrono::steady_clock::now();
  do {
    const std::size_t length =
        static_cast<std::size_t>(std::min<std::uint64_t>(total - offset, kMaximumCiUploadChunkBytes));
    std::string chunk(length, '\0');
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(chunk.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(in.gcount()) != length) throw std::runtime_error("could not read runner upload payload");
    command.offset = offset;
    const CiRemoteTransport::Result result = transport_.run(command, chunk, std::chrono::seconds(300));
    const std::string reply = firstLine(result.output);
    if (result.exit_code == 0 && reply.rfind("ok ", 0) == 0) {
      offset = parseCount(reply.substr(3));
      failures = 0;
    } else if (reply.find("lease-lost") != std::string::npos) {
      lease_lost_ = true;
      throw std::runtime_error("this runner lost its lease on the job");
    } else if (++failures > 6) {
      throw std::runtime_error("the upload failed: " + (reply.empty() ? std::string("no connection") : reply));
    } else {
      // A dropped connection: wait, then resume from whatever the main host
      // holds (an offset-0 retry of the same upload is answered with it).
      std::this_thread::sleep_for(std::chrono::seconds(1u << std::min(failures, 5u)));
      command.offset = 0;
      const CiRemoteTransport::Result probe = transport_.run(command, {}, std::chrono::seconds(60));
      const std::string probe_reply = firstLine(probe.output);
      if (probe.exit_code == 0 && probe_reply.rfind("ok ", 0) == 0) offset = parseCount(probe_reply.substr(3));
    }
    tick();
    maybeHeartbeat(false);
    if (std::chrono::steady_clock::now() - last_progress >= std::chrono::seconds(5) && total > 0) {
      last_progress = std::chrono::steady_clock::now();
      detail_ = "uploading " + name + " (" + std::to_string(offset >> 20) + "/" + std::to_string(total >> 20) + " MiB)";
      send("progress " + hexOrDash(detail_) + "\n");
    }
  } while (offset < total);
}

CiArtifactRecord RemoteCiSink::putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                                           const std::string& note, bool release) {
  CiDeclaredOutput output{artifact.name, release, artifact.retention_days, note};
  CiArtifactRecord provisional;
  provisional.name = artifact.name;
  if (output.note.empty()) {
    try {
      provisional.bytes = std::filesystem::file_size(bundle);
      upload(release ? "asset" : "artifact", artifact.name, bundle);
    } catch (const std::exception& error) {
      output.note = std::string(error.what()).substr(0, 200);
      provisional.bytes = 0;
    }
  }
  provisional.note = output.note;
  outputs_.push_back(std::move(output));
  return provisional;
}

void RemoteCiSink::putPagesSite(const std::filesystem::path& site) {
  const std::filesystem::path archive = relay_dir_ / "site.tar";
  TarPackOptions options;
  options.timeout = std::chrono::seconds(600);
  options.max_bytes = kMaximumPagesSiteBytes;
  options.max_entries = kMaximumPagesEntries + 1;  // include TAR's root directory
  options.skip_non_regular = true;
  options.should_continue = [this] { tick(); return !cancelRequested() && !leaseLost(); };
  packRegularTreeTar(site, {"."}, archive, options);
  try {
    upload("pages", kCiPagesUploadName, archive);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(archive, ignored);
    throw;
  }
  std::error_code error;
  std::filesystem::remove(archive, error);
}

void RemoteCiSink::abandon() {
  CiRunnerCommand command;
  command.op = CiRunnerOp::kAbandon;
  command.job_id = job_.prepared.job.job_id;
  command.token = job_.token;
  transport_.run(command, {}, std::chrono::seconds(30));
  abandoned_ = true;
}

CiRunRecord RemoteCiSink::finish(CiRunRecord record) {
  if (lease_lost_) {
    disconnect();
    record.status = CiRunStatus::Error;
    record.detail = "this runner lost its lease on the job; it was requeued";
    return record;
  }
  if (stopping() && !cancel_) {
    // Shutting down mid-run: hand the job back for another runner instead of
    // reporting a cancel nobody asked for.
    disconnect();
    abandon();
    record.detail = "handed back: the runner is shutting down";
    return record;
  }
  // Let the main host catch up on the logs before the run commits.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    tick();
    flush(true);
    const bool caught_up = std::all_of(steps_.begin(), steps_.end(),
                                       [](const StepLog& step) { return step.acked >= step.size; });
    if ((caught_up && out_buffer_.empty()) || lease_lost_ || std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  disconnect();
  if (lease_lost_) return finish(std::move(record));

  CiFinishReport report;
  report.status = record.status;
  report.started_epoch_seconds = record.started_epoch_seconds;
  report.detail = record.detail;
  report.steps = record.steps;
  report.outputs = outputs_;
  if (transport_.supportsExecutionReport()) report.execution = record.execution;
  CiRunnerCommand command;
  command.op = CiRunnerOp::kFinish;
  command.job_id = job_.prepared.job.job_id;
  command.token = job_.token;
  const std::string payload = formatCiFinishReport(report);
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const CiRemoteTransport::Result result = transport_.run(command, payload, std::chrono::seconds(600));
    const std::string reply = firstLine(result.output);
    if (result.exit_code == 0 && reply.rfind("ok ", 0) == 0) {
      if (const std::optional<CiRunStatus> stored = ciRunStatusFromName(reply.substr(3))) record.status = *stored;
      return record;
    }
    if (reply.find("lease-lost") != std::string::npos) {
      lease_lost_ = true;
      return finish(std::move(record));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1u << std::min(attempt, 5u)));
  }
  record.status = CiRunStatus::Error;
  record.detail = "could not report the result to the main host";
  return record;
}

}  // namespace ckgit
