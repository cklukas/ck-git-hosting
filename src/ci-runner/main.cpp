// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"

namespace {

int usage(std::ostream& out, int code) {
  out << "usage:\n"
         "  ck-ci-runnerd run --repo DIR --commit ID --project NAME --state-root DIR\n"
         "                    --build-root DIR [--ref REF] [--timeout SECONDS]\n"
         "                    [--max-log-bytes N] [--allow-network] [--keep-scratch]\n"
         "                    [--env KEY=VALUE ...]\n"
         "\n"
         "Runs the .ckgit/ci.yml workflow in COMMIT of the bare repository at --repo,\n"
         "each step sandboxed, and records the result under --state-root.\n";
  return code;
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
