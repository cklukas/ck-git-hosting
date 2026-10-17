// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <fstream>
#include <sstream>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "ckgit/ci_gateway.hpp"
#include "ckgit/ci_remote.hpp"
#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/dir_watch.hpp"
#include "ckgit/runtime_status.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/process.hpp"
#include "ckgit/text.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

int usage(std::ostream& out, int code) {
  out << "usage:\n"
         "  ck-ci-runnerd serve --config FILE [--once]\n"
         "  ck-ci-runnerd serve --remote --config RUNNER.INI [--once]\n"
         "  ck-ci-runnerd register --server USER@HOST --name NAME [--config RUNNER.INI]\n"
         "                         [--home DIR] [--account USER] [--force]\n"
         "  ck-ci-runnerd gateway --config FILE --runner ID --command 'ckgit-runner 1 ...'\n"
         "  ck-ci-runnerd run --repo DIR --commit ID --project NAME --state-root DIR\n"
         "                    --build-root DIR [--ref REF] [--timeout SECONDS]\n"
         "                    [--max-log-bytes N] [--allow-network] [--keep-scratch]\n"
         "                    [--env KEY=VALUE ...]\n"
         "\n"
         "serve consumes the CI job spool and runs each job; run executes one job\n"
         "directly. Each step is sandboxed and the result is recorded under the state\n"
         "root (a run.ini plus per-step logs).\n";
  return code;
}

// Consumes the spool: claim the oldest job, resolve its bare repository from the
// configured repo root, run it, then release it. When the spool is empty it
// waits for a change notification on the queue directories (inotify), with a
// slow safety rescan behind it, instead of polling. Stops promptly on
// SIGTERM/SIGINT. With `once`, drains the current spool and returns instead of
// waiting (a cron-style single pass, and how tests drive it).
int serve(const std::filesystem::path& config_path, bool once) {
  const ckgit::ServerConfig config = ckgit::loadServerConfig(config_path);
  if (!config.state_root.has_value() || config.state_root->empty()) {
    std::cerr << "ck-ci-runnerd: serve requires state_root in the configuration\n";
    return 2;
  }
  if (!config.ci_build_root.has_value()) {
    std::cerr << "ck-ci-runnerd: serve requires ci_build_root in the configuration\n";
    return 2;
  }
  const std::filesystem::path state_root = *config.state_root;
  ckgit::recordRuntimeComponent(state_root, "ck-ci-runnerd", ckgit::buildVersion());
  const std::filesystem::path repo_root = config.repo_root;
  const std::filesystem::path build_root = *config.ci_build_root;
  // Where notifications are unavailable the rescan is the only way to notice a
  // job, so it falls back to the old, short poll interval.
  const unsigned fallback_poll = config.ci_poll_seconds.value_or(5);
  const unsigned cleanup_interval = config.ci_cleanup_interval_seconds.value_or(3600);

  const auto nowEpoch = []() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
  };
  std::uint64_t last_sweep = 0;
  const auto maybeSweep = [&](bool force) {
    const std::uint64_t now = nowEpoch();
    if (!force && last_sweep != 0 && now - last_sweep < cleanup_interval) return;
    last_sweep = now;
    ckgit::CiArtifactSweepOptions sweep;
    sweep.now_epoch_seconds = now;
    sweep.max_total_bytes =
        config.ci_artifact_max_total_bytes.value_or(static_cast<unsigned long long>(10) << 30);
    sweep.max_project_bytes =
        config.ci_artifact_max_project_bytes.value_or(static_cast<unsigned long long>(2) << 30);
    sweep.runs_keep = config.ci_runs_keep.value_or(200);
    sweep.keep_latest = config.ci_artifact_keep_latest.value_or(true);
    try {
      ckgit::sweepCiArtifacts(state_root, sweep);
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: artifact sweep failed: " << error.what() << "\n";
    }
  };

  std::signal(SIGTERM, onStop);
  std::signal(SIGINT, onStop);

  ckgit::DirectoryWatch watch;
  try {
    const ckgit::CiQueuePaths queue = ckgit::ensureCiQueueDirectories(state_root);
    watch.add(queue.spool);
    watch.add(queue.working);
  } catch (const std::exception& error) {
    std::cerr << "ck-ci-runnerd: could not prepare the CI queue: " << error.what() << "\n";
  }
  const unsigned rescan = watch.active() ? config.ci_rescan_seconds.value_or(300) : fallback_poll;
  const unsigned lease_seconds = config.ci_lease_seconds.value_or(ckgit::kCiDefaultLeaseSeconds);
  const unsigned max_attempts = config.ci_max_attempts.value_or(ckgit::kCiDefaultMaxAttempts);
  // The primary claims a new job only after it waited this long without a
  // free secondary runner taking it.
  const std::chrono::milliseconds claim_delay(config.ci_claim_delay_ms.value_or(1000));
  std::optional<std::chrono::milliseconds> claim_retry;

  // Requeues jobs whose runner was lost (its lease lapsed). On start-up the
  // primary's own earlier leases count as lapsed too: whatever this runner was
  // doing before it restarted is no longer running.
  std::optional<std::uint64_t> next_lease_expiry;
  const auto reapLeases = [&](bool restarted) {
    try {
      ckgit::CiReapOptions reap;
      reap.max_attempts = max_attempts;
      if (restarted) reap.restarted_runner = ckgit::kCiPrimaryRunnerName;
      const ckgit::CiReapResult result = ckgit::reapCiLeases(state_root, reap);
      next_lease_expiry = result.next_expiry_epoch_seconds;
      for (const std::string& id : result.requeued) {
        std::cout << "ck-ci-runnerd: requeued " << id << " (its runner was lost)\n";
      }
      for (const std::string& id : result.failed) {
        std::cout << "ck-ci-runnerd: gave up on " << id << " (its runner was lost on the last attempt)\n";
      }
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not check CI leases: " << error.what() << "\n";
    }
  };
  reapLeases(true);
  if (!once && !watch.active()) {
    std::cerr << "ck-ci-runnerd: warning: spool notifications unavailable — rescanning every " << rescan
              << "s\n";
  }
  // Waits until the queue changes, the next safety rescan or maintenance sweep
  // is due, or a stop is requested. Waiting happens in short slices only so a
  // signal is noticed promptly; the spool itself is not read while waiting.
  const auto waitForWork = [&]() {
    const auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(rescan);
    if (claim_retry.has_value() && start + *claim_retry < deadline) {
      // A queued job becomes eligible for the primary when its delay is over.
      deadline = start + *claim_retry;
    }
    if (next_lease_expiry.has_value()) {
      // Wake one second after the earliest lease lapses, to requeue it.
      const std::uint64_t now = nowEpoch();
      const auto lapse_in =
          std::chrono::seconds(*next_lease_expiry + 1 > now ? *next_lease_expiry + 1 - now : 0);
      if (start + lapse_in < deadline) deadline = start + lapse_in;
    }
    if (last_sweep != 0) {
      const std::uint64_t now = nowEpoch();
      const std::uint64_t due = last_sweep + cleanup_interval;
      const auto sweep_in = std::chrono::seconds(due > now ? due - now : 0);
      if (start + sweep_in < deadline) deadline = start + sweep_in;
    }
    while (g_stop == 0) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) return;
      if (watch.wait(std::min(remaining, std::chrono::milliseconds(500)))) return;
    }
  };

  bool warned_degraded = false;
  bool warned_loopback = false;
  bool warned_unmasked = false;
  while (g_stop == 0) {
    reapLeases(false);
    std::optional<ckgit::CiJobRequest> job;
    std::string lease_token;
    try {
      ckgit::CiClaimOptions claim_options;
      claim_options.lease_seconds = lease_seconds;
      claim_options.min_age = claim_delay;
      if (std::optional<ckgit::CiClaim> claim = ckgit::claimCiJob(state_root, claim_options, &claim_retry)) {
        job = claim->job;
        lease_token = claim->lease.token;
      }
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not read the spool: " << error.what() << "\n";
    }
    if (!job.has_value()) {
      if (once && !claim_retry.has_value()) {
        maybeSweep(true);  // a final maintenance pass before draining out
        break;
      }
      if (once) {
        // A queued job is still inside its claim delay: drain it too.
        std::this_thread::sleep_for(*claim_retry + std::chrono::milliseconds(10));
        continue;
      }
      maybeSweep(false);
      waitForWork();
      continue;
    }
    ckgit::CiRunnerOptions options;
    options.repository = repo_root / (job->project_name + ".git");
    options.project_name = job->project_name;
    options.ref = job->ref;
    options.commit_id = job->commit_id;
    options.run_id = job->job_id;
    options.attempt = job->attempt;
    options.state_root = state_root;
    options.build_root = build_root;
    options.timeout_seconds = config.ci_timeout_seconds.value_or(1800);
    options.max_log_bytes = config.ci_max_log_bytes.value_or(1u << 20);
    options.allow_network = config.ci_allow_network;
    options.artifact_retention_days = config.ci_artifact_retention_days.value_or(7);
    options.artifact_max_retention_days = config.ci_artifact_max_retention_days.value_or(90);
    options.artifact_max_bytes = static_cast<std::size_t>(
        config.ci_artifact_max_bytes.value_or(static_cast<unsigned long long>(256) << 20));
    options.pages_root = config.pages_root.value_or(std::filesystem::path{});
    options.pages_keep_versions = config.pages_keep_versions.value_or(3);
    options.cache_root = config.ci_cache_root.value_or(std::filesystem::path{});
    {
      const std::string job_id = job->job_id;
      options.keep_alive = [state_root, job_id, lease_token, lease_seconds]() {
        return ckgit::renewCiLease(state_root, job_id, lease_token, lease_seconds);
      };
      options.keep_alive_seconds = std::max(1u, lease_seconds / 6);
      // A stop (a restart or package upgrade) hands the job back to the queue,
      // the same as a remote runner does, rather than failing the run.
      options.stop_requested = []() { return g_stop != 0; };
      options.give_back = [state_root, job_id, lease_token]() {
        return ckgit::abandonCiJob(state_root, job_id, lease_token, ckgit::kCiPrimaryRunnerName);
      };
    }
    if (!config.control_socket.empty()) {
      // Refresh the dashboard as soon as the run publishes itself as Running, so
      // a new run appears within seconds rather than at the next periodic sweep.
      const std::filesystem::path socket = config.control_socket;
      const std::string project = job->project_name;
      options.on_run_started = [socket, project]() {
        try {
          ckgit::forwardControlRpc(socket, "ci-runner", "refresh", {project}, nullptr,
                                   std::chrono::seconds(2));
        } catch (const std::exception&) {
        }
      };
    }
    try {
      ckgit::CiSandboxReport sandbox;
      const ckgit::CiRunRecord record = ckgit::runCiWorkflow(options, &sandbox);
      if (sandbox.probed && !sandbox.namespaces_available && !warned_degraded) {
        std::cerr << "ck-ci-runnerd: warning: unprivileged namespaces unavailable — steps run "
                     "without network/mount isolation\n";
        warned_degraded = true;
      }
      if (sandbox.namespaces_available && !sandbox.loopback_available && !warned_loopback) {
        std::cerr << "ck-ci-runnerd: warning: could not bring up loopback inside the isolated network "
                     "namespace — steps cannot reach 127.0.0.1/::1\n";
        warned_loopback = true;
      }
      if (sandbox.namespaces_available && !sandbox.filesystem_masked && !warned_unmasked) {
        std::cerr << "ck-ci-runnerd: warning: could not mask the service tree inside the mount "
                     "namespace — steps may be able to read and write the state root, other "
                     "projects' releases, and other projects' caches directly\n";
        warned_unmasked = true;
      }
      std::cout << "ck-ci-runnerd: " << job->project_name << " " << record.run_id << " "
                << (record.status == ckgit::CiRunStatus::Pending ? std::string("handed back")
                                                                 : std::string(ckgit::ciRunStatusName(record.status)))
                << "\n";
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: run failed for " << job->project_name << ": " << error.what() << "\n";
    }
    try {
      // Only while this runner still holds the lease: a job it lost or gave
      // back may already belong to another runner.
      ckgit::releaseCiClaim(state_root, job->job_id, lease_token);
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not release job " << job->job_id << ": " << error.what() << "\n";
    }
    if (!config.control_socket.empty()) {
      // Nudge the dashboard to pick up the new run record now, rather than at
      // the next periodic sweep. Best-effort: the sweep is the fallback.
      try {
        ckgit::forwardControlRpc(config.control_socket, "ci-runner", "refresh", {job->project_name},
                                 nullptr, std::chrono::seconds(2));
      } catch (const std::exception&) {
      }
    }
    maybeSweep(false);
  }
  return 0;
}

// Empties a directory without removing it (the runner's build root), making
// read-only directories a step left behind writable first. Returns false when
// something could not be removed.
bool emptyDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return true;
  for (auto entry = std::filesystem::recursive_directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied, error);
       !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error)) {
    if (entry->is_directory(error) && !entry->is_symlink(error)) {
      std::filesystem::permissions(entry->path(), std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
    }
  }
  bool clean = true;
  for (const auto& child : std::filesystem::directory_iterator(directory, error)) {
    std::error_code remove_error;
    std::filesystem::remove_all(child.path(), remove_error);
    if (remove_error) clean = false;
  }
  return clean && std::filesystem::is_empty(directory, error);
}

// A remote runner (see ci_remote.hpp): claims jobs from the main host over
// SSH, runs them in the same sandbox, relays everything back, and keeps
// nothing between jobs. Stops on SIGTERM/SIGINT, handing a job in progress
// back to the queue. With `once`, exits when the queue is empty.
int serveRemote(const std::filesystem::path& config_path, bool once) {
  const ckgit::RunnerConfig config = ckgit::loadRunnerConfig(config_path);
  const ckgit::CiRemoteTransport transport(config, config_path);
  std::signal(SIGTERM, onStop);
  std::signal(SIGINT, onStop);
  std::signal(SIGPIPE, SIG_IGN);
  std::error_code error;
  std::filesystem::create_directories(config.build_root, error);
  ::chmod(config.build_root.c_str(), 0700);
  // A clean runner: whatever a crash or power loss left behind goes first.
  if (!emptyDirectory(config.build_root)) {
    std::cerr << "ck-ci-runnerd: cannot empty the build root " << config.build_root << "\n";
    return 1;
  }
  if (!ckgit::ciSandboxAvailable() && !config.allow_degraded) {
    std::cerr << "ck-ci-runnerd: unprivileged namespaces are unavailable on this host; refusing to run jobs "
                 "without the sandbox (set ci_allow_degraded=true to accept that)\n";
    return 1;
  }
  {
    ckgit::CiRunnerCommand hello;
    hello.op = ckgit::CiRunnerOp::kHello;
    hello.version_hex = ckgit::hexEncode(ckgit::buildVersion());
    const auto result = transport.run(hello, {}, std::chrono::seconds(60));
    if (result.exit_code == 0 && result.output.rfind("ok\n", 0) == 0) {
      std::cout << "ck-ci-runnerd: connected to " << config.server << " as " << config.name << "\n";
    } else {
      std::cerr << "ck-ci-runnerd: could not reach " << config.server << " yet; will keep trying\n";
    }
  }
  unsigned failures = 0;
  while (g_stop == 0) {
    ckgit::CiRunnerCommand claim;
    claim.op = ckgit::CiRunnerOp::kClaim;
    claim.wait_seconds = once ? 0 : config.claim_wait_seconds;
    const auto answer = transport.run(claim, {}, std::chrono::seconds(claim.wait_seconds + 60));
    std::optional<ckgit::CiRemoteJob> job;
    bool ok = answer.exit_code == 0;
    if (ok) {
      try {
        job = ckgit::parseCiClaimResponse(answer.output);
      } catch (const std::exception& parse_error) {
        std::cerr << "ck-ci-runnerd: unusable claim answer: " << parse_error.what() << "\n";
        ok = false;
      }
    }
    if (!ok) {
      // Unreachable main host (or asleep, or rebooting): back off, try again.
      ++failures;
      const unsigned delay = std::min(60u, 1u << std::min(failures, 6u));
      for (unsigned tick = 0; tick < delay * 10 && g_stop == 0; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (once && failures > 3) return 1;
      continue;
    }
    failures = 0;
    if (!job.has_value()) {
      if (once) break;
      continue;  // the long poll ended without work; ask again
    }
    const std::string run_id = job->prepared.job.job_id;
    std::cout << "ck-ci-runnerd: " << job->prepared.job.project_name << " " << run_id << " claimed (attempt "
              << job->prepared.job.attempt << ")\n";
    ckgit::CiExecutionOptions execution;
    execution.runner = config.name;
    execution.build_root = config.build_root;
    execution.timeout_seconds = job->policy.timeout_seconds;
    if (config.timeout_seconds.has_value()) {
      execution.timeout_seconds = std::min(execution.timeout_seconds, *config.timeout_seconds);
    }
    execution.max_log_bytes = static_cast<std::size_t>(job->policy.max_log_bytes);
    execution.allow_network = job->policy.allow_network && config.allow_network;
    execution.artifact_max_bytes = static_cast<std::size_t>(job->policy.artifact_max_bytes);
    execution.cache_root = config.cache_root.value_or(std::filesystem::path{});
    // The runner's own secrets: a step must never read the SSH key it could
    // use to impersonate this runner.
    if (config.identity.has_value()) execution.hidden_paths.push_back(config.identity->parent_path());
    if (config.known_hosts.has_value()) execution.hidden_paths.push_back(config.known_hosts->parent_path());
    ckgit::RemoteCiSource source(transport, run_id, job->token);
    ckgit::RemoteCiSink sink(transport, *job, config.build_root / (run_id + ".relay"), &g_stop);
    try {
      ckgit::CiSandboxReport sandbox;
      const ckgit::CiRunRecord record = ckgit::executeCiJob(job->prepared, execution, source, sink, &sandbox);
      std::cout << "ck-ci-runnerd: " << job->prepared.job.project_name << " " << run_id << " "
                << (sink.abandoned() ? std::string("handed back") : std::string(ckgit::ciRunStatusName(record.status)))
                << "\n";
    } catch (const std::exception& run_error) {
      std::cerr << "ck-ci-runnerd: run failed for " << run_id << ": " << run_error.what() << "\n";
    }
    // Keep nothing: the checkout, sisters, build outputs, packed bundles.
    if (!emptyDirectory(config.build_root)) {
      std::cerr << "ck-ci-runnerd: could not empty the build root after " << run_id << "; not taking new jobs\n";
      return 1;
    }
  }
  return 0;
}

// Prepares this machine as a remote runner (run as root, after installing the
// ck-git-runner package): an SSH key for the runner account, the main host's
// pinned host key, and runner.ini; then prints what the main host's
// administrator runs to admit the key.
int registerRunner(const std::string& server, const std::string& name, const std::filesystem::path& config_path,
                   const std::filesystem::path& home, const std::string& account, bool force) {
  if (!ckgit::isValidSshCloneTarget(server) || !ckgit::isValidClientId(name) || name == ckgit::kCiPrimaryRunnerName) {
    std::cerr << "ck-ci-runnerd: register needs --server user@host and a valid --name\n";
    return 2;
  }
  const struct passwd* user = ::getpwnam(account.c_str());
  if (user == nullptr) {
    std::cerr << "ck-ci-runnerd: the account '" << account << "' does not exist (install the ck-git-runner package)\n";
    return 1;
  }
  const uid_t uid = user->pw_uid;
  const gid_t gid = user->pw_gid;
  const auto own = [&](const std::filesystem::path& path, mode_t mode) {
    if (::chown(path.c_str(), uid, gid) != 0 || ::chmod(path.c_str(), mode) != 0) {
      throw std::runtime_error("could not secure " + path.string());
    }
  };
  if (std::filesystem::exists(config_path) && !force) {
    std::cerr << "ck-ci-runnerd: " << config_path << " already exists (use --force to replace it)\n";
    return 1;
  }
  const std::filesystem::path ssh_dir = home / ".ssh";
  const std::filesystem::path identity = ssh_dir / "id_ed25519";
  const std::filesystem::path known_hosts = ssh_dir / "known_hosts";
  const std::filesystem::path build_root = home / "ci-build";
  std::filesystem::create_directories(ssh_dir);
  std::filesystem::create_directories(build_root);
  own(home, 0700);
  own(ssh_dir, 0700);
  own(build_root, 0700);
  if (!std::filesystem::exists(identity)) {
    const auto keygen = ckgit::runProcess(
        {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "ck-git-runner " + name, "-f", identity.string()},
        std::chrono::seconds(30));
    if (keygen.exit_code != 0) throw std::runtime_error("ssh-keygen failed: " + keygen.output);
  }
  own(identity, 0600);
  own(std::filesystem::path(identity.string() + ".pub"), 0644);
  // Pin the main host's key now, so the runner never accepts another one.
  const std::string host = server.substr(server.find('@') + 1);
  const auto scan = ckgit::runProcess({"ssh-keyscan", "-T", "10", "-t", "ed25519,ecdsa,rsa", host},
                                      std::chrono::seconds(30));
  bool pinned = false;
  if (scan.exit_code == 0 && scan.output.find(' ') != std::string::npos) {
    std::string lines;
    std::istringstream in(scan.output);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.front() != '#') lines += line + "\n";
    }
    if (!lines.empty()) {
      std::ofstream(known_hosts, std::ios::trunc) << lines;
      own(known_hosts, 0644);
      pinned = true;
    }
  }
  ckgit::RunnerConfig config;
  config.server = server;
  config.name = name;
  config.build_root = build_root;
  config.identity = identity;
  if (pinned) config.known_hosts = known_hosts;
  std::filesystem::create_directories(config_path.parent_path());
  std::ofstream(config_path, std::ios::trunc) << ckgit::renderRunnerConfig(config);
  ::chmod(config_path.c_str(), 0644);
  std::ifstream public_key(identity.string() + ".pub");
  std::string key;
  std::getline(public_key, key);
  std::cout << "Wrote " << config_path.string() << (pinned ? "" : " (the main host's key was not pinned: "
                                                                 "it is accepted on first use)")
            << ".\n\nOn the main host, admit this runner's key (as root):\n\n"
            << "  printf '%s\\n' '" << key << "' > /tmp/" << name << ".pub\n"
            << "  sudo -u ckgit ckgit-admin runner add " << name << " --public-key /tmp/" << name
            << ".pub --config /etc/ck-git-hosting/server.ini >> /etc/ck-git-hosting/authorized_keys\n\n"
            << "Then start the runner here:\n\n  systemctl enable --now ck-git-runner.service\n";
  return 0;
}

std::string need(int& index, int argc, char** argv, const std::string& option) {
  if (++index >= argc) throw std::runtime_error(option + " requires a value");
  return argv[index];
}

}  // namespace

int main(int argc, char** argv) {
  // Under systemd stdout is a pipe to the journal: line-buffer it so each
  // progress line (connected, claimed, finished) appears when it happens.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    if (argc < 2) return usage(std::cerr, 2);
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") return usage(std::cout, 0);
    if (command == "--version" || command == "-V") { std::cout << ckgit::versionLine("ck-ci-runnerd"); return 0; }
    if (command == "serve") {
      std::filesystem::path config_path;
      bool once = false;
      bool remote = false;
      for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--config") config_path = need(i, argc, argv, option);
        else if (option == "--once") once = true;
        else if (option == "--remote") remote = true;
        else throw std::runtime_error("unknown serve option: " + option);
      }
      if (config_path.empty()) {
        std::cerr << "ck-ci-runnerd: serve requires --config FILE\n";
        return usage(std::cerr, 2);
      }
      return remote ? serveRemote(config_path, once) : serve(config_path, once);
    }
    if (command == "register") {
      std::string server;
      std::string name;
      std::filesystem::path config_path = "/etc/ck-git-hosting/runner.ini";
      std::filesystem::path home = "/var/lib/ck-git-runner";
      std::string account = "ckgit-runner";
      bool force = false;
      for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--server") server = need(i, argc, argv, option);
        else if (option == "--name") name = need(i, argc, argv, option);
        else if (option == "--config") config_path = need(i, argc, argv, option);
        else if (option == "--home") home = need(i, argc, argv, option);
        else if (option == "--account") account = need(i, argc, argv, option);
        else if (option == "--force") force = true;
        else throw std::runtime_error("unknown register option: " + option);
      }
      return registerRunner(server, name, config_path, home, account, force);
    }
    if (command == "ssh-shim") {
      // What GIT_SSH_COMMAND runs while a remote runner fetches a job's source.
      std::filesystem::path config_path;
      std::string job_id;
      std::string token;
      int i = 2;
      for (; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--config") config_path = argv[i + 1];
        else if (option == "--job") job_id = argv[i + 1];
        else if (option == "--token") token = argv[i + 1];
        else break;
      }
      std::vector<std::string> git_arguments(argv + i, argv + argc);
      return ckgit::runCiSshShim(config_path, job_id, token, git_arguments);
    }
    if (command == "gateway") {
      // Started by ck-git-shell for a runner key; see ci_gateway.hpp.
      std::filesystem::path config_path;
      std::string runner;
      std::string line;
      for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--config") config_path = need(i, argc, argv, option);
        else if (option == "--runner") runner = need(i, argc, argv, option);
        else if (option == "--command") line = need(i, argc, argv, option);
        else throw std::runtime_error("unknown gateway option: " + option);
      }
      std::string reason;
      const std::optional<ckgit::CiRunnerCommand> parsed = ckgit::parseCiRunnerCommand(line, &reason);
      if (config_path.empty() || runner.empty() || !parsed.has_value()) {
        std::cout << "error " << (reason.empty() ? "invalid gateway invocation" : reason) << "\n";
        return 2;
      }
      std::signal(SIGPIPE, SIG_IGN);
      return ckgit::runCiGateway(ckgit::loadServerConfig(config_path), runner, *parsed, STDIN_FILENO, STDOUT_FILENO);
    }
    if (command != "run") {
      std::cerr << "ck-ci-runnerd: unknown command '" << command << "'\n";
      return usage(std::cerr, 2);
    }

    ckgit::CiRunnerOptions options;
    options.ref = "refs/heads/main";
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--repo") options.repository = need(i, argc, argv, option);
      else if (option == "--commit") options.commit_id = need(i, argc, argv, option);
      else if (option == "--project") options.project_name = need(i, argc, argv, option);
      else if (option == "--ref") options.ref = need(i, argc, argv, option);
      else if (option == "--state-root") options.state_root = need(i, argc, argv, option);
      else if (option == "--build-root") options.build_root = need(i, argc, argv, option);
      else if (option == "--cache-root") options.cache_root = need(i, argc, argv, option);
      else if (option == "--timeout") options.timeout_seconds = std::stoul(need(i, argc, argv, option));
      else if (option == "--max-log-bytes") options.max_log_bytes = std::stoul(need(i, argc, argv, option));
      else if (option == "--allow-network") options.allow_network = true;
      else if (option == "--keep-scratch") options.keep_scratch = true;
      else if (option == "--env") options.extra_env.push_back(need(i, argc, argv, option));
      else throw std::runtime_error("unknown option: " + option);
    }
    if (options.repository.empty() || options.commit_id.empty() || options.project_name.empty() ||
        options.state_root.empty() || options.build_root.empty()) {
      std::cerr << "ck-ci-runnerd: --repo, --commit, --project, --state-root and --build-root are required\n";
      return usage(std::cerr, 2);
    }

    ckgit::CiSandboxReport sandbox;
    const ckgit::CiRunRecord record = ckgit::runCiWorkflow(options, &sandbox);

    if (!sandbox.probed) {
      // The run ended before anything executed (skipped or cancelled).
    } else if (!sandbox.namespaces_available) {
      std::cerr << "ck-ci-runnerd: warning: unprivileged namespaces unavailable — steps ran without "
                   "network/mount isolation"
                << (ckgit::ciSandboxCompiledIn() ? " (kernel setting?)" : " (not a Linux host)") << "\n";
    } else if (!sandbox.network_isolated) {
      std::cerr << "ck-ci-runnerd: note: network access was enabled for this run\n";
    } else if (!sandbox.loopback_available) {
      std::cerr << "ck-ci-runnerd: warning: could not bring up loopback inside the isolated network "
                   "namespace — steps could not reach 127.0.0.1/::1\n";
    }
    if (sandbox.namespaces_available && !sandbox.filesystem_masked) {
      std::cerr << "ck-ci-runnerd: warning: could not mask the service tree inside the mount "
                   "namespace — steps may have been able to read and write the state root, other "
                   "projects' releases, and other projects' caches directly\n";
    }

    std::cout << "run " << record.run_id << ": " << ckgit::ciRunStatusName(record.status);
    if (!record.detail.empty()) std::cout << " (" << record.detail << ")";
    std::cout << "\n";
    for (const ckgit::CiStepResult& step : record.steps) {
      std::cout << "  step " << step.name << ": exit " << step.exit_code
                << (step.timed_out ? " [timeout]" : "") << (step.output_truncated ? " [log truncated]" : "")
                << "\n";
    }

    switch (record.status) {
      case ckgit::CiRunStatus::Success:
      case ckgit::CiRunStatus::Skipped:
        return 0;
      default:
        return 1;
    }
  } catch (const std::exception& error) {
    std::cerr << "ck-ci-runnerd: " << error.what() << "\n";
    return 2;
  }
}
