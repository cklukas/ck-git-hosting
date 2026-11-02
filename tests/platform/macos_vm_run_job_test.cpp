// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "../../src/ci-vm-host-macos/run_job.hpp"
#include "../../src/ci-vm-guest-macos/result_sink.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace {
volatile sig_atomic_t fail_stop = 0;
const char* stop_marker = nullptr;
void stopped(int) {
  if (fail_stop) kill(getpid(), SIGKILL);
  if (stop_marker) {
    const int fd = open(stop_marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) close(fd);
  }
  _exit(0);
}
struct Source : ckgit::CiSourceProvider {
  bool* cancel = nullptr;
  void checkout(const ckgit::CiCheckoutRequest&, const fs::path& path) override {
    std::ofstream(path / "source") << "ok";
    if (cancel) *cancel = true;
  }
};
struct Sink : ckgit::CiResultSink {
  std::string log; bool completed = false, cancelled = false, lost = false;
  fs::path cancel_when_ready;
  fs::path lose_when_stopped;
  fs::path fail_heartbeat_when_stopped;
  void runStarted(const ckgit::CiRunRecord&) override {}
  void stepStarted(const ckgit::CiRunRecord&, std::size_t) override {}
  void appendLog(std::size_t, std::string_view value) override { log += value; }
  void stepFinished(const ckgit::CiRunRecord&, std::size_t) override {}
  void heartbeat(const ckgit::CiRunRecord&) override {
    if (!cancel_when_ready.empty() && fs::exists(cancel_when_ready)) cancelled = true;
    if (!lose_when_stopped.empty() && fs::exists(lose_when_stopped)) lost = true;
    if (!fail_heartbeat_when_stopped.empty() && fs::exists(fail_heartbeat_when_stopped))
      throw std::runtime_error("simulated heartbeat failure");
  }
  void progress(const ckgit::CiRunRecord&) override {}
  bool cancelRequested() override { return cancelled; }
  bool leaseLost() override { return lost; }
  ckgit::CiArtifactRecord putArtifact(const ckgit::CiArtifact&, const fs::path&, const std::string&, bool) override { return {}; }
  void putPagesSite(const fs::path&) override {}
  ckgit::CiRunRecord finish(ckgit::CiRunRecord record) override { completed = true; return record; }
};
}
int main(int argc, char** argv) {
  if (argc == 5 && std::strcmp(argv[1], "run") == 0) {
    fail_stop = std::getenv("CKGIT_TEST_VM_FAIL_STOP") != nullptr;
    stop_marker = std::getenv("CKGIT_TEST_VM_STOP_MARKER");
    struct sigaction action {}; action.sa_handler = stopped; sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, nullptr) != 0) return 2;
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGTERM); sigaddset(&signals, SIGINT);
    if (sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) return 2;
    if (const char* ready = std::getenv("CKGIT_TEST_VM_STALL_READY")) {
      std::ofstream(ready) << "ready";
      for (;;) pause();
    }
    try {
      ckgit::CiVmChannel channel(STDIN_FILENO);
      const auto assignment = ckgit::ciVmGuestHandshake(channel, std::chrono::seconds(5));
      ckgit::CiVmGuestSink sink(channel, assignment.prepared, assignment.policy, argv[3]);
      ckgit::CiRunRecord record; record.started_epoch_seconds = 1;
      sink.runStarted(record); sink.stepStarted(record, 0); sink.appendLog(0, "mock job");
      record.steps.push_back({"build", 0, false, false}); sink.stepFinished(record, 0);
      record.status = ckgit::CiRunStatus::Success; sink.finish(record);
      for (;;) pause();
    } catch (...) { return 3; }
  }
  const char* tmp = std::getenv("TMPDIR"); if (!tmp) return 1;
  std::string pattern = (fs::path(tmp) / "vm-run-job-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
  if (!mkdtemp(name.data())) return 1;
  const fs::path root(name.data());
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  try {
    fs::create_directory(root / "base"); fs::create_directory(root / "work"); fs::create_directory(root / "relay");
    chmod((root / "base").c_str(), 0700);
    chmod((root / "work").c_str(), 0700);
    for (const char* file : {"Disk.img", "AuxiliaryStorage", "MachineIdentifier", "HardwareModel"}) {
      std::ofstream(root / "base" / file) << "fixture";
      chmod((root / "base" / file).c_str(), 0600);
    }
    ckgit::CiPreparedJob job;
    job.job.job_id = "00000000000000000001-abcdabcd";
    job.job.project_name = "project"; job.job.commit_id = std::string(40, 'a'); job.job.ref = "refs/heads/main";
    job.workflow_text = "version: 1\njobs:\n  - name: build\n    steps:\n      - run: [true]\n";
    job.workflow = ckgit::parseCiWorkflow(job.workflow_text);
    Source source; Sink sink; ckgit::CiRemotePolicy policy;
    ckgit::CiMacosVmOptions options{fs::canonical(argv[0]), root / "base", root / "work", root / "relay", 1};
    Sink early; early.cancelled = true;
    const auto early_result = ckgit::runCiMacosVmJob(job, policy, source, early, options);
    if (early_result.status != ckgit::CiRunStatus::Cancelled || !early.completed || !fs::is_empty(root / "work"))
      throw std::runtime_error("pre-start VM cancellation was not finalized and cleaned");
    Sink during; Source cancelling; cancelling.cancel = &during.cancelled;
    const auto during_result = ckgit::runCiMacosVmJob(job, policy, cancelling, during, options);
    if (during_result.status != ckgit::CiRunStatus::Cancelled || !during.completed || !fs::is_empty(root / "work"))
      throw std::runtime_error("source staging cancellation was not finalized and cleaned");
    Sink lost; lost.cancelled = true; lost.lost = true;
    bool lost_rejected = false;
    try { (void)ckgit::runCiMacosVmJob(job, policy, source, lost, options); }
    catch (const std::exception&) { lost_rejected = true; }
    if (!lost_rejected || lost.completed || !fs::is_empty(root / "work"))
      throw std::runtime_error("lost lease published a cancellation or left a workspace");
    Sink boot_cancel; boot_cancel.cancel_when_ready = root / "mock-helper-ready";
    if (setenv("CKGIT_TEST_VM_STALL_READY", boot_cancel.cancel_when_ready.c_str(), 1) != 0)
      throw std::runtime_error("cannot set boot cancellation fixture");
    const auto boot_result = ckgit::runCiMacosVmJob(job, policy, source, boot_cancel, options);
    unsetenv("CKGIT_TEST_VM_STALL_READY");
    if (boot_result.status != ckgit::CiRunStatus::Cancelled || !boot_cancel.completed ||
        !fs::exists(boot_cancel.cancel_when_ready) || !fs::is_empty(root / "work"))
      throw std::runtime_error("boot cancellation was not stopped, finalized and cleaned");
    Sink shutdown_lost; shutdown_lost.lose_when_stopped = root / "mock-helper-stopped";
    if (setenv("CKGIT_TEST_VM_STOP_MARKER", shutdown_lost.lose_when_stopped.c_str(), 1) != 0)
      throw std::runtime_error("cannot set shutdown lease fixture");
    bool shutdown_rejected = false;
    try { (void)ckgit::runCiMacosVmJob(job, policy, source, shutdown_lost, options); }
    catch (const std::exception&) { shutdown_rejected = true; }
    unsetenv("CKGIT_TEST_VM_STOP_MARKER");
    if (!shutdown_rejected || !shutdown_lost.lost || shutdown_lost.completed || !fs::is_empty(root / "work"))
      throw std::runtime_error("shutdown lease loss published a result or left a workspace");
    Sink heartbeat_error; heartbeat_error.fail_heartbeat_when_stopped = root / "mock-heartbeat-failure";
    if (setenv("CKGIT_TEST_VM_STOP_MARKER", heartbeat_error.fail_heartbeat_when_stopped.c_str(), 1) != 0)
      throw std::runtime_error("cannot set shutdown heartbeat failure fixture");
    bool heartbeat_rejected = false;
    try { (void)ckgit::runCiMacosVmJob(job, policy, source, heartbeat_error, options); }
    catch (const std::exception&) { heartbeat_rejected = true; }
    unsetenv("CKGIT_TEST_VM_STOP_MARKER");
    if (!heartbeat_rejected || heartbeat_error.completed || !fs::is_empty(root / "work"))
      throw std::runtime_error("heartbeat failure published a result or prevented safe cleanup");
    const auto result = ckgit::runCiMacosVmJob(job, policy, source, sink, options);
    if (result.status != ckgit::CiRunStatus::Success || sink.log != "mock job" || !sink.completed ||
        !fs::is_empty(root / "work")) throw std::runtime_error("VM job orchestration failed");
    Sink failed_sink;
    if (setenv("CKGIT_TEST_VM_FAIL_STOP", "1", 1) != 0) throw std::runtime_error("cannot set failure fixture");
    bool rejected = false;
    try { (void)ckgit::runCiMacosVmJob(job, policy, source, failed_sink, options); }
    catch (const std::exception&) { rejected = true; }
    unsetenv("CKGIT_TEST_VM_FAIL_STOP");
    if (!rejected || failed_sink.completed || fs::is_empty(root / "work"))
      throw std::runtime_error("failed VM shutdown published success or removed the workspace");
    Sink restart_sink;
    bool quarantined = false;
    try { (void)ckgit::runCiMacosVmJob(job, policy, source, restart_sink, options); }
    catch (const std::exception& e) {
      quarantined = std::string(e.what()).find("workspace from this boot") != std::string::npos;
    }
    if (!quarantined || restart_sink.completed ||
        std::distance(fs::directory_iterator(root / "work"), fs::directory_iterator{}) != 1)
      throw std::runtime_error("restart did not quarantine the interrupted VM workspace");
    std::cout << "VM job orchestration passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
