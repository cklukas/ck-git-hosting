// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/runtime_status.hpp"
#include "ckgit/server_config.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

int usage(std::ostream& out, int code) {
  out << "usage:\n"
         "  ck-ci-runnerd serve --config FILE [--once]\n"
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
// configured repo root, run it, then release it; sleep when the spool is empty.
// Stops promptly on SIGTERM/SIGINT. With `once`, drains the current spool and
// returns instead of waiting (a cron-style single pass, and how tests drive it).
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
  const unsigned poll = config.ci_poll_seconds.value_or(5);
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

  bool warned_degraded = false;
  bool warned_loopback = false;
  bool warned_unmasked = false;
  while (g_stop == 0) {
    std::optional<ckgit::CiJobRequest> job;
    try {
      job = ckgit::claimNextCiJob(state_root);
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not read the spool: " << error.what() << "\n";
    }
    if (!job.has_value()) {
      if (once) {
        maybeSweep(true);  // a final maintenance pass before draining out
        break;
      }
      maybeSweep(false);
      for (unsigned tick = 0; tick < poll * 10 && g_stop == 0; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }
    ckgit::CiRunnerOptions options;
    options.repository = repo_root / (job->project_name + ".git");
    options.project_name = job->project_name;
    options.ref = job->ref;
    options.commit_id = job->commit_id;
    options.run_id = job->job_id;
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
    if (!config.control_socket.empty()) {
      // Refresh the dashboard as soon as the run publishes itself as Running, so
      // a new run appears within seconds rather than at the next periodic sweep.
      const std::filesystem::path socket = config.control_socket;
      const std::string project = job->project_name;
      options.on_run_started = [socket, project]() {
        try {
          ckgit::forwardControlRpc(socket, "ci-runner", "refresh", project, {}, nullptr,
                                   std::chrono::seconds(2));
        } catch (const std::exception&) {
        }
      };
    }
    try {
      ckgit::CiSandboxReport sandbox;
      const ckgit::CiRunRecord record = ckgit::runCiWorkflow(options, &sandbox);
      if (!sandbox.namespaces_available && !warned_degraded) {
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
                << ckgit::ciRunStatusName(record.status) << "\n";
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: run failed for " << job->project_name << ": " << error.what() << "\n";
    }
    try {
      ckgit::releaseCiJob(state_root, job->job_id);
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not release job " << job->job_id << ": " << error.what() << "\n";
    }
    if (!config.control_socket.empty()) {
      // Nudge the dashboard to pick up the new run record now, rather than at
      // the next periodic sweep. Best-effort: the sweep is the fallback.
      try {
        ckgit::forwardControlRpc(config.control_socket, "ci-runner", "refresh", job->project_name, {},
                                 nullptr, std::chrono::seconds(2));
      } catch (const std::exception&) {
      }
    }
    maybeSweep(false);
  }
  return 0;
}

std::string need(int& index, int argc, char** argv, const std::string& option) {
  if (++index >= argc) throw std::runtime_error(option + " requires a value");
  return argv[index];
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) return usage(std::cerr, 2);
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") return usage(std::cout, 0);
    if (command == "--version" || command == "-V") { std::cout << ckgit::versionLine("ck-ci-runnerd"); return 0; }
    if (command == "serve") {
      std::filesystem::path config_path;
      bool once = false;
      for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--config") config_path = need(i, argc, argv, option);
        else if (option == "--once") once = true;
        else throw std::runtime_error("unknown serve option: " + option);
      }
      if (config_path.empty()) {
        std::cerr << "ck-ci-runnerd: serve requires --config FILE\n";
        return usage(std::cerr, 2);
      }
      return serve(config_path, once);
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

    if (!sandbox.namespaces_available) {
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
