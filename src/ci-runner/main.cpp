// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <csignal>
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
#include "ckgit/control_rpc.hpp"
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
  const std::filesystem::path repo_root = config.repo_root;
  const std::filesystem::path build_root = *config.ci_build_root;
  const unsigned poll = config.ci_poll_seconds.value_or(5);

  std::signal(SIGTERM, onStop);
  std::signal(SIGINT, onStop);

  bool warned_degraded = false;
  while (g_stop == 0) {
    std::optional<ckgit::CiJobRequest> job;
    try {
      job = ckgit::claimNextCiJob(state_root);
    } catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd: could not read the spool: " << error.what() << "\n";
    }
    if (!job.has_value()) {
      if (once) break;  // the spool is drained
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
    options.state_root = state_root;
    options.build_root = build_root;
    options.timeout_seconds = config.ci_timeout_seconds.value_or(1800);
    options.max_log_bytes = config.ci_max_log_bytes.value_or(1u << 20);
    options.allow_network = config.ci_allow_network;
    try {
      ckgit::CiSandboxReport sandbox;
      const ckgit::CiRunRecord record = ckgit::runCiWorkflow(options, &sandbox);
      if (!sandbox.namespaces_available && !warned_degraded) {
        std::cerr << "ck-ci-runnerd: warning: unprivileged namespaces unavailable — steps run "
                     "without network/mount isolation\n";
        warned_degraded = true;
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
