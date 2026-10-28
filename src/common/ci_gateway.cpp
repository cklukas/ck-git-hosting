// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_gateway.hpp"

#include <algorithm>
#include <iostream>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/ci_results.hpp"
#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/dir_watch.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

std::uint64_t nowEpoch() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void writeAll(int fd, std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("the runner connection closed");
    offset += static_cast<std::size_t>(written);
  }
}

// Reads `fd` to its end, refusing more than `cap` bytes.
std::string readBounded(int fd, std::size_t cap) {
  std::string data;
  char buffer[65536];
  for (;;) {
    const ssize_t received = ::read(fd, buffer, sizeof(buffer));
    if (received < 0 && errno == EINTR) continue;
    if (received < 0) throw std::runtime_error("could not read the request payload");
    if (received == 0) return data;
    if (data.size() + static_cast<std::size_t>(received) > cap) throw std::runtime_error("the request payload is too large");
    data.append(buffer, static_cast<std::size_t>(received));
  }
}

int refuse(int out, const std::string& reason) {
  try {
    writeAll(out, "error " + reason + "\n");
  } catch (const std::exception&) {
  }
  return 1;
}

std::filesystem::path stateRoot(const ServerConfig& config) {
  if (!config.state_root.has_value()) throw std::runtime_error("the server has no state_root");
  return *config.state_root;
}

std::filesystem::path repositoryOf(const ServerConfig& config, const std::string& project) {
  return config.repo_root / (project + ".git");
}

unsigned leaseSeconds(const ServerConfig& config) {
  return config.ci_lease_seconds.value_or(kCiDefaultLeaseSeconds);
}

CiRemotePolicy policyOf(const ServerConfig& config) {
  CiRemotePolicy policy;
  policy.timeout_seconds = config.ci_timeout_seconds.value_or(1800);
  policy.max_log_bytes = config.ci_max_log_bytes.value_or(1u << 20);
  policy.artifact_max_bytes = config.ci_artifact_max_bytes.value_or(static_cast<unsigned long long>(256) << 20);
  policy.allow_network = config.ci_allow_network;
  policy.lease_seconds = leaseSeconds(config);
  return policy;
}

CiCommitPolicy commitPolicyOf(const ServerConfig& config, const std::string& project) {
  CiCommitPolicy policy;
  policy.state_root = stateRoot(config);
  policy.repository = repositoryOf(config, project);
  policy.pages_root = config.pages_root.value_or(std::filesystem::path{});
  policy.pages_keep_versions = config.pages_keep_versions.value_or(3);
  policy.artifact_retention_days = config.ci_artifact_retention_days.value_or(7);
  policy.artifact_max_retention_days = config.ci_artifact_max_retention_days.value_or(90);
  policy.artifact_max_bytes = static_cast<std::size_t>(
      config.ci_artifact_max_bytes.value_or(static_cast<unsigned long long>(256) << 20));
  return policy;
}

// Asks the dashboard to reload a project now rather than at its next sweep.
void refresh(const ServerConfig& config, const std::string& project) {
  if (config.control_socket.empty()) return;
  try {
    forwardControlRpc(config.control_socket, "ci-runner", "refresh", {project}, nullptr, std::chrono::seconds(2));
  } catch (const std::exception&) {
  }
}

void recordEvent(const ServerConfig& config, std::string_view kind, const std::string& project,
                 const std::string& runner) {
  try {
    appendStateEvent(validatedMetadataRoot(stateRoot(config)), kind, project, runner);
  } catch (const std::exception&) {
  }
}

// The claim behind a request, when it belongs to this runner under this token.
std::optional<CiClaim> heldClaim(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command) {
  std::optional<CiClaim> claim = loadCiClaim(stateRoot(config), command.job_id);
  if (!claim.has_value() || claim->lease.token != command.token || claim->lease.runner != runner) return std::nullopt;
  if (!claim->job.parent_run_id.empty() && command.protocol_version < 2) return std::nullopt;
  return claim;
}

CiPreparedJob preparedOrMinimal(const ServerConfig& config, const CiJobRequest& job) {
  CiPreparation preparation = prepareCiJob(repositoryOf(config, job.project_name), job);
  if (preparation.prepared.has_value()) return std::move(*preparation.prepared);
  // The workflow can no longer be read (the repository changed under the
  // run): commit what the runner reported, publishing no release or site.
  CiPreparedJob minimal;
  minimal.job = job;
  return minimal;
}

int hello(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int out) {
  try {
    const std::filesystem::path state = stateRoot(config);
    static_cast<void>(validatedMetadataRoot(state));
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
  std::string version;
  if (std::optional<std::string> decoded = hexDecode(command.version_hex, 128)) version = *decoded;
  noteCiRunnerSeen(stateRoot(config), runner, version);
  writeAll(out, "ok\nprotocol=" + std::to_string(command.protocol_version) + "\nrunner=" + runner +
                    "\nserver_version_hex=" + hexEncode(buildVersion()) + "\nexecution_report=1\n");
  return 0;
}

int claim(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int out) {
  const std::filesystem::path state = stateRoot(config);
  DirectoryWatch watch;
  try {
    const CiQueuePaths queue = ensureCiQueueDirectories(state);
    watch.add(queue.spool);
    watch.add(queue.working);
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(command.wait_seconds);
  noteCiRunnerSeen(state, runner, {});
  CiClaimOptions options;
  options.runner = runner;
  options.protocol_version = command.protocol_version;
  options.lease_seconds = leaseSeconds(config);
  for (;;) {
    try {
      CiReapOptions reap;
      reap.max_attempts = config.ci_max_attempts.value_or(kCiDefaultMaxAttempts);
      reapCiLeases(state, reap);
      for (const auto& error : recoverCiWorkflowResults(commitPolicyOf(config, "unused"), config.repo_root))
        std::cerr << "ci workflow publication: " << error << "\n";
    } catch (const std::exception&) {
    }
    std::optional<CiClaim> claimed;
    // A disabled runner is drained: it finishes what it holds, claims nothing.
    const std::optional<CiRunnerRecord> registered = loadCiRunnerRecord(state, runner);
    const bool enabled = !registered.has_value() || registered->enabled;
    // Its labels come from the server's registry, never from the runner.
    options.labels = registered.has_value() ? registered->labels : std::vector<std::string>{};
    try {
      if (enabled) claimed = claimCiJob(state, options);
    } catch (const std::exception& error) {
      return refuse(out, error.what());
    }
    if (claimed.has_value()) {
      const CiJobRequest& job = claimed->job;
      // Jobs that end without running are finished right here, on the main
      // host: a cancel while queued, no workflow, a ref that is no trigger.
      if (isCiCancelRequested(state, job.project_name, job.job_id)) {
        recordCiOutcome(state, job, CiRunStatus::Cancelled, "cancelled before it started");
        finalizeCiParentForChild(commitPolicyOf(config, job.project_name), job);
        releaseCiJob(state, job.job_id);
        refresh(config, job.project_name);
        continue;
      }
      const CiPreparation preparation = prepareCiJob(repositoryOf(config, job.project_name), job);
      if (!preparation.prepared.has_value()) {
        recordCiOutcome(state, job, preparation.status, preparation.detail);
        finalizeCiParentForChild(commitPolicyOf(config, job.project_name), job);
        releaseCiJob(state, job.job_id);
        refresh(config, job.project_name);
        continue;
      }
      recordEvent(config, "ci-claimed", job.project_name, runner);
      writeAll(out, formatCiClaimResponse(*preparation.prepared, claimed->lease, policyOf(config), command.protocol_version));
      return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) break;
    // Waits on the queue's change notification, not on a timer; the slices
    // only bound how long a lost notification could delay the claim.
    watch.wait(std::min(remaining, std::chrono::milliseconds(5000)));
  }
  writeAll(out, "none\n");
  return 0;
}

int uploadPack(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  if (!claim.has_value()) return refuse(out, "lease-lost");
  // Only the job's own project and the sisters its workflow declares.
  bool allowed = command.project == claim->job.project_name;
  if (!allowed) {
    const CiPreparedJob prepared = preparedOrMinimal(config, claim->job);
    allowed = std::any_of(prepared.sisters.begin(), prepared.sisters.end(),
                          [&](const CiResolvedSister& sister) { return sister.name == command.project; });
  }
  if (!allowed) return refuse(out, "project is not part of this job");
  const std::filesystem::path repository = repositoryOf(config, command.project);
  std::error_code error;
  if (!std::filesystem::is_directory(repository, error)) return refuse(out, "repository is unavailable");
  // Any reachable commit may be fetched by hash: the run needs exactly its
  // commit (and its sisters' resolved commits) even after a branch moved on.
  ::execlp("git", "git", "-c", "uploadpack.allowAnySHA1InWant=true", "upload-pack", "--strict",
           repository.c_str(), static_cast<char*>(nullptr));
  return refuse(out, std::string("could not start git-upload-pack: ") + std::strerror(errno));
}

int heartbeat(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  const std::filesystem::path state = stateRoot(config);
  if (!claim.has_value() || !renewCiLease(state, command.job_id, command.token, leaseSeconds(config))) {
    writeAll(out, "lease-lost\n");
    return 0;
  }
  noteCiRunnerSeen(state, runner, {});
  writeAll(out, isCiCancelRequested(state, claim->job.project_name, claim->job.job_id) ? "cancel\n" : "ok\n");
  return 0;
}

int put(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int in, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  if (!claim.has_value()) return refuse(out, "lease-lost");
  const std::optional<CiUploadKind> kind = ciUploadKindFromName(command.kind);
  if (!kind.has_value()) return refuse(out, "unknown upload kind");
  try {
    const std::string chunk = readBounded(in, kMaximumCiUploadChunkBytes);
    const std::uint64_t cap = *kind == CiUploadKind::kPages
                                  ? static_cast<std::uint64_t>(kMaximumPagesSiteBytes)
                                  : config.ci_artifact_max_bytes.value_or(static_cast<unsigned long long>(256) << 20);
    const std::uint64_t held =
        appendCiStagedUpload(stateRoot(config), claim->job.project_name, claim->job.job_id, *kind, command.name,
                             command.total, command.sha256, command.offset, chunk, cap);
    renewCiLease(stateRoot(config), command.job_id, command.token, leaseSeconds(config));
    writeAll(out, "ok " + std::to_string(held) + "\n");
    return 0;
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
}

int finish(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int in, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  if (!claim.has_value()) return refuse(out, "lease-lost");
  const std::filesystem::path state = stateRoot(config);
  const CiJobRequest& job = claim->job;
  try {
    const CiFinishReport report = parseCiFinishReport(readBounded(in, kMaximumCiFinishPayloadBytes));
    CiRunRecord record;
    if (std::optional<CiRunRecord> existing = loadCiRun(state, job.project_name, job.job_id)) record = *existing;
    record.run_id = job.job_id;
    record.parent_run_id = job.parent_run_id;
    record.workflow_job = job.workflow_job;
    record.project_name = job.project_name;
    record.ref = job.ref;
    record.commit_id = job.commit_id;
    record.runner = runner;
    record.attempt = job.attempt;
    record.status = report.status;
    if (report.started_epoch_seconds != 0) record.started_epoch_seconds = report.started_epoch_seconds;
    record.detail = report.detail.substr(0, kMaximumCiDetailBytes);
    record.steps = report.steps;
    record.execution = report.execution;
    record.finished_epoch_seconds = nowEpoch();
    record.heartbeat_epoch_seconds = record.finished_epoch_seconds;
    const CiPreparedJob prepared = preparedOrMinimal(config, job);
    const auto prior = loadCiRun(state, job.project_name, job.job_id);
    const bool settled_child = !job.parent_run_id.empty() && prior && !ciRunStatusIsActive(prior->status);
    const auto policy = commitPolicyOf(config, job.project_name);
    const CiRunRecord stored = settled_child ? *prior :
        commitCiRun(policy, prepared, std::move(record), report.outputs);
    finalizeCiParentForChild(policy, job);
    releaseCiClaim(state, job.job_id, claim->lease.token);
    recordEvent(config, "ci-finished", job.project_name, runner);
    refresh(config, job.project_name);
    writeAll(out, "ok " + std::string(ciRunStatusName(stored.status)) + "\n");
    return 0;
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
}

int abandon(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  if (!claim.has_value()) return refuse(out, "lease-lost");
  if (!abandonCiJob(stateRoot(config), command.job_id, command.token, runner)) return refuse(out, "lease-lost");
  recordEvent(config, "ci-requeued", claim->job.project_name, runner);
  refresh(config, claim->job.project_name);
  writeAll(out, "ok\n");
  return 0;
}

// --- stream: a run's live progress, relayed into its record and logs --------

class StreamSession {
 public:
  StreamSession(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command,
                const CiClaim& claim, int out)
      : config_(config), runner_(runner), command_(command), job_(claim.job), out_(out), state_(stateRoot(config)) {
    max_log_ = config.ci_max_log_bytes.value_or(1u << 20);
    if (std::optional<CiRunRecord> existing = loadCiRun(state_, job_.project_name, job_.job_id)) {
      record_ = *existing;
    } else {
      record_.run_id = job_.job_id;
      record_.project_name = job_.project_name;
      record_.ref = job_.ref;
      record_.commit_id = job_.commit_id;
      record_.status = CiRunStatus::Pending;
    }
    record_.runner = runner;
    record_.attempt = job_.attempt;
    run_dir_ = prepareCiRunDirectory(state_, job_.project_name, job_.job_id);
  }

  // Tells the runner how much of each step log is already held, so it can
  // resend exactly the rest after a reconnect.
  void greet() {
    std::string greeting;
    for (std::size_t index = 0; index <= record_.steps.size(); ++index) {
      std::error_code error;
      const auto size = std::filesystem::file_size(logPath(index), error);
      if (!error) greeting += "held " + std::to_string(index) + " " + std::to_string(size) + "\n";
    }
    greeting += "ready\n";
    writeAll(out_, greeting);
  }

  // Consumes complete frames from `buffer`; returns false to end the session.
  bool consume(std::string& buffer) {
    for (;;) {
      const std::size_t newline = buffer.find('\n');
      if (newline == std::string::npos) {
        if (buffer.size() > 4096) throw std::runtime_error("a stream frame header is too long");
        return true;
      }
      const std::string header = buffer.substr(0, newline);
      std::vector<std::string> tokens;
      std::size_t start = 0;
      for (;;) {
        const std::size_t space = header.find(' ', start);
        tokens.push_back(header.substr(start, space == std::string::npos ? std::string::npos : space - start));
        if (space == std::string::npos) break;
        start = space + 1;
      }
      if (tokens[0] == "log") {
        if (tokens.size() != 4) throw std::runtime_error("malformed log frame");
        const std::size_t length = static_cast<std::size_t>(parse(tokens[3]));
        if (length > kMaximumCiLogFrameBytes) throw std::runtime_error("a log frame is too large");
        if (buffer.size() < newline + 1 + length) return true;  // wait for the payload
        appendLog(static_cast<std::size_t>(parse(tokens[1])), parse(tokens[2]),
                  std::string_view(buffer).substr(newline + 1, length));
        buffer.erase(0, newline + 1 + length);
        continue;
      }
      buffer.erase(0, newline + 1);
      if (!frame(tokens)) return false;
    }
  }

  // Once a second: relay a cancel request, and stop when the lease is gone.
  bool check() {
    if (!cancel_sent_ && isCiCancelRequested(state_, job_.project_name, job_.job_id)) {
      writeAll(out_, "cancel\n");
      cancel_sent_ = true;
    }
    const std::optional<CiClaim> claim = loadCiClaim(state_, job_.job_id);
    if (!claim.has_value() || claim->lease.token != command_.token) {
      writeAll(out_, "lease-lost\n");
      return false;
    }
    return true;
  }

 private:
  static std::uint64_t parse(const std::string& token) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || error != std::errc{} || end != token.data() + token.size()) {
      throw std::runtime_error("malformed stream number");
    }
    return value;
  }

  std::filesystem::path logPath(std::size_t index) const {
    return run_dir_ / "steps" / (std::to_string(index) + ".log");
  }

  void write(bool force) {
    const std::uint64_t now = nowEpoch();
    if (!force && now - last_write_ < 3) return;
    last_write_ = now;
    record_.heartbeat_epoch_seconds = now;
    writeCiRunRecord(state_, record_);
  }

  void appendLog(std::size_t index, std::uint64_t offset, std::string_view bytes) {
    if (index > record_.steps.size() || index > kMaximumCiSteps) throw std::runtime_error("a log for an unknown step");
    const std::filesystem::path path = logPath(index);
    std::error_code error;
    const auto existing = std::filesystem::file_size(path, error);
    const std::uint64_t size = error ? 0 : existing;
    if (offset > size) {
      // A gap: tell the runner where the held bytes end, so it resends.
      writeAll(out_, "held " + std::to_string(index) + " " + std::to_string(size) + "\n");
      return;
    }
    const std::uint64_t skip = size - offset;  // bytes of this frame already held
    if (skip < bytes.size() && size < max_log_) {
      std::string_view fresh = bytes.substr(static_cast<std::size_t>(skip));
      if (size + fresh.size() > max_log_) fresh = fresh.substr(0, static_cast<std::size_t>(max_log_ - size));
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) throw std::runtime_error("could not open a step log");
      std::size_t done = 0;
      while (done < fresh.size()) {
        const ssize_t written = ::write(fd, fresh.data() + done, fresh.size() - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        done += static_cast<std::size_t>(written);
      }
      ::close(fd);
    }
    const auto now_size = std::filesystem::file_size(path, error);
    // Acknowledge what the runner sent even past the cap: those bytes are
    // deliberately dropped, never to be resent.
    const std::uint64_t acknowledged = std::max<std::uint64_t>(error ? 0 : now_size, offset + bytes.size());
    writeAll(out_, "ack " + std::to_string(index) + " " + std::to_string(acknowledged) + "\n");
  }

  bool frame(const std::vector<std::string>& tokens) {
    const std::string& kind = tokens[0];
    if (kind == "started" && tokens.size() == 2) {
      record_.status = CiRunStatus::Running;
      const std::uint64_t started = parse(tokens[1]);
      record_.started_epoch_seconds = started != 0 ? started : nowEpoch();
      record_.finished_epoch_seconds = 0;
      write(true);
      refresh(config_, job_.project_name);
    } else if (kind == "execution" && tokens.size() == 2) {
      if (tokens[1] == "-") record_.execution.reset();
      else record_.execution = parseCiExecutionReport(tokens[1]);
      write(true);
    } else if (kind == "step-start" && tokens.size() == 3) {
      const std::size_t index = static_cast<std::size_t>(parse(tokens[1]));
      if (index == record_.steps.size()) {
        const int fd = ::open(logPath(index).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) ::close(fd);
        record_.status = CiRunStatus::Running;
        write(true);
      }
    } else if (kind == "step-end" && tokens.size() == 6) {
      const std::size_t index = static_cast<std::size_t>(parse(tokens[1]));
      if (index == record_.steps.size() && index < kMaximumCiSteps) {
        CiStepResult step;
        const std::string& exit_text = tokens[2];
        const bool negative = !exit_text.empty() && exit_text.front() == '-';
        step.exit_code = static_cast<int>(parse(negative ? exit_text.substr(1) : exit_text));
        if (negative) step.exit_code = -step.exit_code;
        step.timed_out = tokens[3] == "1";
        step.output_truncated = tokens[4] == "1";
        const std::optional<std::string> name = fromHexOrDash(tokens[5], 256);
        if (!name.has_value()) throw std::runtime_error("malformed step name");
        step.name = *name;
        record_.steps.push_back(std::move(step));
        write(true);
        refresh(config_, job_.project_name);
      }
    } else if (kind == "heartbeat" && tokens.size() == 1) {
      if (!renewCiLease(state_, job_.job_id, command_.token, leaseSeconds(config_))) {
        writeAll(out_, "lease-lost\n");
        return false;
      }
      write(false);
      // A busy runner is in contact too: keep its last-seen current for the
      // runners page, at most once a minute.
      const std::uint64_t now = nowEpoch();
      if (now - last_seen_note_ >= 60) {
        last_seen_note_ = now;
        noteCiRunnerSeen(state_, runner_, {});
      }
    } else if (kind == "progress" && tokens.size() == 2) {
      const std::optional<std::string> detail = fromHexOrDash(tokens[1], kMaximumCiDetailBytes);
      if (!detail.has_value()) throw std::runtime_error("malformed progress");
      record_.detail = *detail;
      write(true);
    } else {
      throw std::runtime_error("unknown stream frame '" + kind + "'");
    }
    return true;
  }

  const ServerConfig& config_;
  std::string runner_;
  const CiRunnerCommand& command_;
  CiJobRequest job_;
  int out_;
  std::filesystem::path state_;
  std::filesystem::path run_dir_;
  CiRunRecord record_;
  std::uint64_t max_log_ = 0;
  std::uint64_t last_write_ = 0;
  std::uint64_t last_seen_note_ = 0;
  bool cancel_sent_ = false;
};

int stream(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int in, int out) {
  const std::optional<CiClaim> claim = heldClaim(config, runner, command);
  if (!claim.has_value()) {
    writeAll(out, "lease-lost\n");
    return 0;
  }
  try {
    StreamSession session(config, runner, command, *claim, out);
    session.greet();
    std::string buffer;
    auto last_check = std::chrono::steady_clock::now();
    char chunk[65536];
    for (;;) {
      pollfd descriptor{in, POLLIN, 0};
      const int ready = ::poll(&descriptor, 1, 1000);
      if (ready < 0 && errno != EINTR) break;
      if (ready > 0) {
        const ssize_t received = ::read(in, chunk, sizeof(chunk));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;  // the runner closed the stream
        buffer.append(chunk, static_cast<std::size_t>(received));
        if (!session.consume(buffer)) return 0;
      }
      if (std::chrono::steady_clock::now() - last_check >= std::chrono::seconds(1)) {
        last_check = std::chrono::steady_clock::now();
        if (!session.check()) return 0;
      }
    }
    return 0;
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
}

}  // namespace

int runCiGateway(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int in,
                 int out) {
  if (!isValidClientId(runner) || runner == kCiPrimaryRunnerName) return refuse(out, "invalid runner name");
  try {
    switch (command.op) {
      case CiRunnerOp::kHello: return hello(config, runner, command, out);
      case CiRunnerOp::kClaim: return claim(config, runner, command, out);
      case CiRunnerOp::kUploadPack: return uploadPack(config, runner, command, out);
      case CiRunnerOp::kStream: return stream(config, runner, command, in, out);
      case CiRunnerOp::kHeartbeat: return heartbeat(config, runner, command, out);
      case CiRunnerOp::kPut: return put(config, runner, command, in, out);
      case CiRunnerOp::kFinish: return finish(config, runner, command, in, out);
      case CiRunnerOp::kAbandon: return abandon(config, runner, command, out);
    }
  } catch (const std::exception& error) {
    return refuse(out, error.what());
  }
  return refuse(out, "unknown operation");
}

}  // namespace ckgit
