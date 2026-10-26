// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include "ckgit/process.hpp"
#include <vector>

#include "ckgit/ci_runner.hpp"
#include "ckgit/runner_config.hpp"
#include "ckgit/ci_runner_protocol.hpp"

namespace ckgit {

// A remote runner: a machine that runs CI jobs for a main ck-git host it
// reaches over SSH, with no repositories, state, or dashboard of its own. It
// claims jobs through the runner protocol (ci_runner_protocol.hpp), fetches
// source through the same SSH account, relays its progress live, uploads its
// outputs, and keeps nothing once a job has ended.

// Runs runner protocol operations over SSH. Every operation is one `ssh` exec
// of the runner key's forced command; a ControlMaster connection under the
// build root is shared between them so they cost no new handshake.
class CiRemoteTransport {
 public:
  CiRemoteTransport(RunnerConfig config, std::filesystem::path config_path, unsigned protocol_version = 1);
  // Negotiate before claiming work; keep that version for the whole lease,
  // including the separate Git SSH shim process. Failed greetings never claim.
  bool negotiate(std::string_view version_hex);
  unsigned protocolVersion() const { return protocol_version_; }

  struct Result {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
  };
  // Runs `command`, writing `input` to its stdin, and collects its stdout.
  Result run(const CiRunnerCommand& command, std::string_view input, std::chrono::seconds timeout) const;

  // A long-lived operation (stream) with both pipes open.
  using Session = std::unique_ptr<ProcessStream>;
  Session open(const CiRunnerCommand& command) const;
  static void close(Session& session);

  // The ssh argv up to (and including) the destination; the remote command
  // line follows as the last argument.
  std::vector<std::string> sshArguments() const;

  // GIT_SSH_COMMAND for fetching a leased job's source through the gateway.
  std::string gitSshCommand(const std::string& job_id, const std::string& token) const;
  // The URL a git fetch uses for `project` with that command.
  std::string fetchUrl(const std::string& project) const;

  const RunnerConfig& config() const { return config_; }

 private:
  RunnerConfig config_;
  std::filesystem::path config_path_;
  unsigned protocol_version_ = 1;
};

// `ck-ci-runnerd ssh-shim`: what GIT_SSH_COMMAND runs. Git calls it as
// `<shim> [ssh options] <host> "git-upload-pack '<project>.git'"`; the shim
// turns that into the gateway's lease-scoped `upload-pack` and executes ssh.
// Returns only on failure.
int runCiSshShim(const std::filesystem::path& config_path, const std::string& job_id, const std::string& token,
                 const std::vector<std::string>& git_arguments, unsigned protocol_version = 1);

// Fetches source through the gateway (see gitSshCommand).
class RemoteCiSource : public CiSourceProvider {
 public:
  RemoteCiSource(const CiRemoteTransport& transport, std::string job_id, std::string token);
  void checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination) override;

 private:
  const CiRemoteTransport& transport_;
  std::string job_id_;
  std::string token_;
};

// Relays a run to the main host: progress and step logs live over a `stream`
// session (each step's output also kept in a local relay file until the main
// host acknowledges it, and resent after a reconnect), outputs as resumable
// `put` uploads, and the end as `finish`. `relay_dir` holds the relay files and
// packed outputs; the caller removes it with the rest of the job's data.
class RemoteCiSink : public CiResultSink {
 public:
  RemoteCiSink(const CiRemoteTransport& transport, const CiRemoteJob& job, std::filesystem::path relay_dir,
               const volatile int* stop_flag);
  ~RemoteCiSink() override;
  void runStarted(const CiRunRecord& record) override;
  void stepStarted(const CiRunRecord& record, std::size_t index) override;
  void appendLog(std::size_t index, std::string_view bytes) override;
  void stepFinished(const CiRunRecord& record, std::size_t index) override;
  void heartbeat(const CiRunRecord& record) override;
  void tick() override;
  void progress(const CiRunRecord& record) override;
  bool cancelRequested() override;
  bool leaseLost() override;
  CiArtifactRecord putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                               const std::string& note, bool release) override;
  void putPagesSite(const std::filesystem::path& site) override;
  CiRunRecord finish(CiRunRecord record) override;

  // True when the job was handed back (shutdown) instead of finished.
  bool abandoned() const { return abandoned_; }

 private:
  struct StepLog {
    std::uint64_t size = 0;    // bytes in the local relay file
    std::uint64_t sent = 0;    // bytes written to the stream
    std::uint64_t acked = 0;   // bytes the main host holds
    bool ended = false;
    CiStepResult result;
  };
  bool connect();
  void disconnect();
  void send(const std::string& frame);
  void pump();
  void flushStep(std::size_t index, bool force);
  void flush(bool force);
  void readReplies();
  void handleReply(const std::string& line);
  void resend();
  void maybeHeartbeat(bool force);
  std::filesystem::path relayPath(std::size_t index) const;
  void upload(const std::string& kind, const std::string& name, const std::filesystem::path& file);
  void abandon();

  const CiRemoteTransport& transport_;
  const CiRemoteJob& job_;
  std::filesystem::path relay_dir_;
  const volatile int* stop_flag_;
  CiRemoteTransport::Session session_{};
  bool connected_ = false;
  std::chrono::steady_clock::time_point last_connect_attempt_{};
  std::chrono::steady_clock::time_point last_flush_{};
  std::chrono::steady_clock::time_point last_heartbeat_{};
  std::string reply_buffer_;
  std::string out_buffer_;
  std::vector<StepLog> steps_;
  std::uint64_t started_epoch_ = 0;
  std::string detail_;
  bool cancel_ = false;
  bool lease_lost_ = false;
  bool abandoned_ = false;
  std::vector<CiDeclaredOutput> outputs_;
};

}  // namespace ckgit
