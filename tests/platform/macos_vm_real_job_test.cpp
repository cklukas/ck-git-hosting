// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Explicit opt-in test against a prepared guest base. Never a production runner.
#include "../../src/ci-vm-host-macos/run_job.hpp"
#include "ckgit/archive.hpp"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
struct Source : ckgit::CiSourceProvider {
  fs::path probe;
  std::chrono::steady_clock::time_point* checkout_done = nullptr;
  explicit Source(fs::path binary) : probe(std::move(binary)) {}
  void checkout(const ckgit::CiCheckoutRequest&, const fs::path& path) override {
    std::ofstream(path / "input") << "fixture";
    fs::copy_file(probe, path / "fd-probe");
    if (checkout_done) *checkout_done = std::chrono::steady_clock::now();
  }
};
struct Sink : ckgit::CiResultSink {
  fs::path root; std::string log, output; bool completed = false, cancel_on_log = false, lose_lease_on_log = false;
  std::chrono::steady_clock::time_point checkout_done {};
  bool cancel_during_boot = false;
  bool cancel_immediately_after_spawn = false;
  unsigned heartbeats_after_checkout = 0;
  explicit Sink(fs::path p, bool cancel = false, bool lose_lease = false)
      : root(std::move(p)), cancel_on_log(cancel), lose_lease_on_log(lose_lease) {}
  void runStarted(const ckgit::CiRunRecord&) override {}
  void stepStarted(const ckgit::CiRunRecord&, std::size_t) override {}
  void appendLog(std::size_t, std::string_view text) override { log += text; }
  void stepFinished(const ckgit::CiRunRecord&, std::size_t) override {}
  void heartbeat(const ckgit::CiRunRecord&) override {
    if (checkout_done != std::chrono::steady_clock::time_point{}) ++heartbeats_after_checkout;
  }
  void progress(const ckgit::CiRunRecord&) override {}
  bool cancelRequested() override {
    return (cancel_on_log && log.find("cancel-probe-ready") != std::string::npos) ||
           (cancel_during_boot && checkout_done != std::chrono::steady_clock::time_point{} &&
            ((cancel_immediately_after_spawn && heartbeats_after_checkout >= 4) ||
             (!cancel_immediately_after_spawn && std::chrono::steady_clock::now() - checkout_done >= std::chrono::seconds(3))));
  }
  bool leaseLost() override { return lose_lease_on_log && log.find("cancel-probe-ready") != std::string::npos; }
  ckgit::CiArtifactRecord putArtifact(const ckgit::CiArtifact& artifact, const fs::path& file,
                                       const std::string& note, bool) override {
    if (!note.empty()) throw std::runtime_error("guest did not produce its artifact");
    ckgit::TarExtractOptions options; options.max_bytes = 1024 * 1024; options.max_archive_bytes = 1024 * 1024;
    options.max_file_bytes = 1024 * 1024;
    ckgit::extractRegularTreeTar(file, root / "artifact", options);
    std::ifstream input(root / "artifact/result.txt"); output.assign(std::istreambuf_iterator<char>(input), {});
    ckgit::CiArtifactRecord result; result.name = artifact.name; return result;
  }
  void putPagesSite(const fs::path&) override { throw std::runtime_error("unexpected Pages output"); }
  ckgit::CiRunRecord finish(ckgit::CiRunRecord record) override { completed = true; return record; }
};
}
int main(int argc, char** argv) {
  if ((argc != 6 && argc != 7) || (argc == 7 && std::string_view(argv[6]) != "--check-tool-env" &&
                                    std::string_view(argv[6]) != "--cancel" && std::string_view(argv[6]) != "--timeout" &&
                                    std::string_view(argv[6]) != "--lease-loss" && std::string_view(argv[6]) != "--isolation" &&
                                    std::string_view(argv[6]) != "--cancel-boot" && std::string_view(argv[6]) != "--cancel-boot-immediate")) {
    std::cerr << "usage: test VM-HELPER BASE WORK-ROOT RELAY-ROOT FD-PROBE [--check-tool-env|--cancel|--cancel-boot|--cancel-boot-immediate|--timeout|--lease-loss|--isolation]\n"; return 2;
  }
  try {
    const bool cancel = argc == 7 && std::string_view(argv[6]) == "--cancel";
    const bool cancel_immediate = argc == 7 && std::string_view(argv[6]) == "--cancel-boot-immediate";
    const bool cancel_boot = cancel_immediate || (argc == 7 && std::string_view(argv[6]) == "--cancel-boot");
    const bool timeout = argc == 7 && std::string_view(argv[6]) == "--timeout";
    const bool lose_lease = argc == 7 && std::string_view(argv[6]) == "--lease-loss";
    const bool isolation = argc == 7 && std::string_view(argv[6]) == "--isolation";
    const bool tool_env = argc == 7 && !cancel && !cancel_boot && !timeout && !lose_lease && !isolation;
    ckgit::CiPreparedJob job;
    job.job.job_id = "00000000000000000001-abcdabcd";
    job.job.project_name = "project"; job.job.commit_id = std::string(40, 'a'); job.job.ref = "refs/heads/main";
    job.workflow_text = "version: 1\n";
    if (tool_env) job.workflow_text += "env: { VM_TEST_TOOL: workflow-value }\n";
    job.workflow_text += "jobs:\n  - name: build\n    steps:\n      - script: |\n          ./fd-probe";
    job.workflow_text += isolation ? " --offline\n" : "\n";
    if (tool_env) job.workflow_text += "          test \"$VM_TEST_TOOL\" = /opt/fixture-tool\n";
    if (isolation) {
      const auto sentinel = (fs::path(argv[4]) / "host-only-sentinel").string();
      if (sentinel.find_first_of("'\r\n") != std::string::npos || fs::exists(sentinel))
        throw std::runtime_error("invalid or preexisting isolation sentinel");
      std::ofstream(sentinel) << "host-only";
      job.workflow_text += "          test -f input\n          test ! -e '" + sentinel + "'\n";
      job.workflow_text += "          if /usr/bin/touch '/Volumes/My Shared Files/ckgit-readonly-probe' 2>/dev/null; then\n"
                           "            /bin/rm '/Volumes/My Shared Files/ckgit-readonly-probe'\n"
                           "            exit 1\n          fi\n";
    }
    if (cancel || timeout || lose_lease) job.workflow_text += "          echo cancel-probe-ready\n          sleep 120\n";
    else job.workflow_text += "          printf vm-build > result.txt\n    artifacts:\n      name: vm-output\n      paths: [result.txt]\n";
    job.workflow = ckgit::parseCiWorkflow(job.workflow_text);
    Source source(argv[5]); Sink sink(argv[4], cancel, lose_lease); ckgit::CiRemotePolicy policy;
    if (cancel_boot) { sink.cancel_during_boot = true; sink.cancel_immediately_after_spawn = cancel_immediate; source.checkout_done = &sink.checkout_done; }
    if (timeout) policy.timeout_seconds = 2;
    ckgit::CiMacosVmOptions options{argv[1], argv[2], argv[3], argv[4], 1};
    ckgit::CiRunRecord result;
    try { result = ckgit::runCiMacosVmJob(job, policy, source, sink, options); }
    catch (const std::exception&) {
      if (!lose_lease || !sink.leaseLost() || sink.completed || !fs::is_empty(argv[3])) throw;
      std::cout << "Real offline VM lease loss and cleanup passed\n"; return 0;
    }
    if (lose_lease) throw std::runtime_error("VM returned a result after losing its lease");
    const auto expected = (cancel || cancel_boot) ? ckgit::CiRunStatus::Cancelled : timeout ? ckgit::CiRunStatus::Timeout : ckgit::CiRunStatus::Success;
    if (result.status != expected || !sink.completed ||
        (!cancel && !cancel_boot && !timeout && sink.output != "vm-build") || ((cancel || cancel_boot) && !sink.cancelRequested()) ||
        (cancel_boot && result.detail != "cancelled before the VM guest was ready") ||
        (timeout && (result.steps.size() != 1 || !result.steps[0].timed_out)) ||
        !fs::is_empty(argv[3])) {
      std::cerr << "VM result: " << ckgit::ciRunStatusName(result.status) << ": " << result.detail << "\n" << sink.log;
      throw std::runtime_error("real VM job or artifact did not pass");
    }
    if (isolation) {
      std::ifstream sentinel(fs::path(argv[4]) / "host-only-sentinel");
      std::string value; std::getline(sentinel, value);
      if (value != "host-only") throw std::runtime_error("host-only sentinel changed");
    }
    std::cout << (cancel_boot ? "Real offline VM boot cancellation and cleanup passed\n" : cancel ? "Real offline VM cancellation and cleanup passed\n" : timeout ?
                           "Real offline VM timeout and cleanup passed\n" : isolation ?
                           "Real offline VM isolation and artifact passed\n" :
                           "Real offline VM CI job and artifact passed\n"); return 0;
  } catch (const std::exception& e) { std::cerr << "real VM test: " << e.what() << '\n'; return 1; }
}
