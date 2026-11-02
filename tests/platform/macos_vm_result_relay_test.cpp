// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "../../src/ci-vm-guest-macos/result_sink.hpp"
#include "../../src/ci-vm-host-macos/result_relay.hpp"
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Sink : ckgit::CiResultSink {
  std::string log, artifact, pages;
  bool finished = false;
  bool cancel_on_log = false, cancelled = false;
  std::atomic<bool> cancel_on_tick{false}, lose_on_tick{false}, lost{false};
  void runStarted(const ckgit::CiRunRecord&) override {}
  void stepStarted(const ckgit::CiRunRecord&, std::size_t) override {}
  void appendLog(std::size_t, std::string_view value) override { log += value; if (cancel_on_log) cancelled = true; }
  void stepFinished(const ckgit::CiRunRecord&, std::size_t) override {}
  void heartbeat(const ckgit::CiRunRecord&) override {}
  void tick() override { if (cancel_on_tick.exchange(false)) cancelled = true; if (lose_on_tick.exchange(false)) lost = true; }
  void progress(const ckgit::CiRunRecord&) override {}
  bool cancelRequested() override { return cancelled; }
  bool leaseLost() override { return lost.load(); }
  ckgit::CiArtifactRecord putArtifact(const ckgit::CiArtifact& declared, const fs::path& file,
                                       const std::string& note, bool) override {
    std::ifstream input(file); artifact.assign(std::istreambuf_iterator<char>(input), {});
    ckgit::CiArtifactRecord result; result.name = declared.name; result.note = note; return result;
  }
  void putPagesSite(const fs::path& path) override {
    std::ifstream input(path / "index.html"); pages.assign(std::istreambuf_iterator<char>(input), {});
  }
  ckgit::CiRunRecord finish(ckgit::CiRunRecord record) override { finished = true; return record; }
};
}
int main() {
  const char* tmp = std::getenv("TMPDIR"); if (!tmp) return 1;
  std::string pattern = (fs::path(tmp) / "vm-relay-XXXXXX").string();
  std::vector<char> chars(pattern.begin(), pattern.end()); chars.push_back('\0');
  if (!mkdtemp(chars.data())) return 1;
  const fs::path root(chars.data());
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  int pair[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
  try {
    ckgit::CiPreparedJob job;
    job.job.job_id = "00000000000000000001-abcdabcd";
    job.job.project_name = "project"; job.job.commit_id = std::string(40, 'a');
    ckgit::CiJob build; build.name = "build"; build.steps.push_back({});
    build.artifact = ckgit::CiArtifact{"output", {"build"}, 7}; job.workflow.jobs.push_back(build);
    job.workflow.pages_path = "site"; job.is_default_branch = true;
    ckgit::CiRemotePolicy policy; ckgit::CiVmChannel host(pair[0]), guest(pair[1]); Sink stored;
    std::exception_ptr host_error;
    std::jthread server([&] {
      try { (void)ckgit::relayCiVmResults(host, job, policy, stored, root); }
      catch (...) { host_error = std::current_exception(); shutdown(pair[0], SHUT_RDWR); }
    });
    ckgit::CiVmGuestSink relay(guest, job, policy, root);
    ckgit::CiRunRecord record; record.started_epoch_seconds = 1;
    relay.runStarted(record); relay.stepStarted(record, 0); relay.appendLog(0, "hello");
    record.steps.push_back({"build", 0, false, false}); relay.stepFinished(record, 0);
    std::ofstream(root / "bundle") << "data";
    relay.putArtifact(*job.workflow.jobs[0].artifact, root / "bundle", {}, false);
    fs::create_directory(root / "site"); std::ofstream(root / "site/index.html") << "page";
    relay.putPagesSite(root / "site");
    record.status = ckgit::CiRunStatus::Success; relay.finish(record);
    server.join(); if (host_error) std::rethrow_exception(host_error);
    require(stored.log == "hello" && stored.artifact == "data" && stored.pages == "page" && !stored.finished,
            "VM result relay lost data");
    close(pair[0]); close(pair[1]);
    int hostile_pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, hostile_pair) == 0, "cannot create hostile pair");
    ckgit::CiVmChannel strict_host(hostile_pair[0]), hostile(hostile_pair[1]);
    policy.max_log_bytes = 2; Sink restricted; bool rejected = false;
    std::jthread strict_server([&] {
      try { (void)ckgit::relayCiVmResults(strict_host, job, policy, restricted, root); }
      catch (...) { rejected = true; shutdown(hostile_pair[0], SHUT_RDWR); }
    });
    const auto timeout = std::chrono::seconds(3);
    hostile.send(ckgit::CiVmFrameType::Event, "started 1", timeout); (void)hostile.receive(timeout);
    hostile.send(ckgit::CiVmFrameType::Event, "step-start 0", timeout); (void)hostile.receive(timeout);
    hostile.send(ckgit::CiVmFrameType::Event, "log 0", timeout);
    hostile.send(ckgit::CiVmFrameType::Chunk, "big", timeout);
    strict_server.join();
    require(rejected && strict_host.failed() && restricted.log.empty(), "oversized guest log was not rejected");
    close(hostile_pair[0]); close(hostile_pair[1]);
    int cancel_pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, cancel_pair) == 0, "cannot create cancel pair");
    ckgit::CiVmChannel cancel_host(cancel_pair[0]), cancel_guest(cancel_pair[1]);
    Sink cancelled_sink; cancelled_sink.cancel_on_log = true; std::exception_ptr cancel_error;
    std::jthread cancel_server([&] {
      try { (void)ckgit::relayCiVmResults(cancel_host, job, policy, cancelled_sink, root); }
      catch (...) { cancel_error = std::current_exception(); shutdown(cancel_pair[0], SHUT_RDWR); }
    });
    ckgit::CiVmGuestSink cancel_relay(cancel_guest, job, policy, root);
    ckgit::CiRunRecord cancelled_record; cancelled_record.started_epoch_seconds = 1;
    cancel_relay.runStarted(cancelled_record); cancel_relay.stepStarted(cancelled_record, 0);
    cancel_relay.appendLog(0, "ok");
    require(cancel_relay.cancelRequested(), "guest did not receive host cancellation");
    cancelled_record.steps.push_back({"build", 143, false, false});
    cancel_relay.stepFinished(cancelled_record, 0);
    cancelled_record.status = ckgit::CiRunStatus::Cancelled; cancel_relay.finish(cancelled_record);
    cancel_server.join(); if (cancel_error) std::rethrow_exception(cancel_error);
    require(!cancelled_sink.finished, "VM result was committed before shutdown");
    close(cancel_pair[0]); close(cancel_pair[1]);
    int ignored_pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, ignored_pair) == 0, "cannot create ignored-cancel pair");
    ckgit::CiVmChannel ignored_host(ignored_pair[0]), ignored_guest(ignored_pair[1]);
    Sink ignored_sink; ignored_sink.cancel_on_log = true; bool ignored_rejected = false;
    std::jthread ignored_server([&] {
      try { (void)ckgit::relayCiVmResults(ignored_host, job, policy, ignored_sink, root); }
      catch (...) { ignored_rejected = true; shutdown(ignored_pair[0], SHUT_RDWR); }
    });
    ignored_guest.send(ckgit::CiVmFrameType::Event, "started 1", timeout); (void)ignored_guest.receive(timeout);
    ignored_guest.send(ckgit::CiVmFrameType::Event, "step-start 0", timeout); (void)ignored_guest.receive(timeout);
    ignored_guest.send(ckgit::CiVmFrameType::Event, "log 0", timeout);
    ignored_guest.send(ckgit::CiVmFrameType::Chunk, "ok", timeout);
    require(ignored_guest.receive(timeout).type == ckgit::CiVmFrameType::Cancel, "host did not send cancellation");
    ignored_guest.send(ckgit::CiVmFrameType::Event, "step-end 0 0 0 0 6275696c64", timeout);
    (void)ignored_guest.receive(timeout);
    ckgit::CiFinishReport ignored_report; ignored_report.status = ckgit::CiRunStatus::Success;
    ignored_report.started_epoch_seconds = 1; ignored_report.steps.push_back({"build", 0, false, false});
    ignored_guest.send(ckgit::CiVmFrameType::Done, ckgit::formatCiFinishReport(ignored_report), timeout);
    ignored_server.join();
    require(ignored_rejected && ignored_host.failed() && !ignored_sink.finished,
            "host accepted success after sending cancellation");
    close(ignored_pair[0]); close(ignored_pair[1]);
    // The state can change in the final acknowledgement's tick, after the
    // terminal frame was received but before it is returned to the runner.
    for (bool lose : {false, true}) {
      int last_pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, last_pair) == 0, "cannot create final-event pair");
      ckgit::CiVmChannel last_host(last_pair[0]), last_guest(last_pair[1]);
      Sink last_sink; bool last_rejected = false; ckgit::CiRunRecord last_result;
      std::jthread last_server([&] {
        try { last_result = ckgit::relayCiVmResults(last_host, job, policy, last_sink, root); }
        catch (...) { last_rejected = true; shutdown(last_pair[0], SHUT_RDWR); }
      });
      last_guest.send(ckgit::CiVmFrameType::Event, "started 1", timeout); (void)last_guest.receive(timeout);
      last_guest.send(ckgit::CiVmFrameType::Event, "step-start 0", timeout); (void)last_guest.receive(timeout);
      last_guest.send(ckgit::CiVmFrameType::Event, "step-end 0 0 0 0 6275696c64", timeout); (void)last_guest.receive(timeout);
      if (lose) last_sink.lose_on_tick = true; else last_sink.cancel_on_tick = true;
      ckgit::CiFinishReport report; report.status = ckgit::CiRunStatus::Success;
      report.started_epoch_seconds = 1; report.steps.push_back({"build", 0, false, false});
      last_guest.send(ckgit::CiVmFrameType::Done, ckgit::formatCiFinishReport(report), timeout);
      const auto response = last_guest.receive(timeout);
      last_server.join();
      if (lose) require(response.type == ckgit::CiVmFrameType::Error && last_rejected && !last_sink.finished,
                        "lease loss during final acknowledgement returned a result");
      else require(response.type == ckgit::CiVmFrameType::Cancel && !last_rejected &&
                   last_result.status == ckgit::CiRunStatus::Cancelled && !last_sink.finished,
                   "late cancellation was reported as success");
      close(last_pair[0]); close(last_pair[1]);
    }
    int early_pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, early_pair) == 0, "cannot create early-artifact pair");
    ckgit::CiVmChannel early_host(early_pair[0]), early_guest(early_pair[1]);
    Sink early_sink; bool early_rejected = false;
    std::jthread early_server([&] {
      try { (void)ckgit::relayCiVmResults(early_host, job, policy, early_sink, root); }
      catch (...) { early_rejected = true; shutdown(early_pair[0], SHUT_RDWR); }
    });
    early_guest.send(ckgit::CiVmFrameType::Event, "started 1", timeout); (void)early_guest.receive(timeout);
    early_guest.send(ckgit::CiVmFrameType::Event, "artifact 0 1 -", timeout);
    early_server.join();
    require(early_rejected && early_host.failed() && early_sink.artifact.empty() && !early_sink.finished,
            "host accepted an artifact before its job ran");
    close(early_pair[0]); close(early_pair[1]);
    std::cout << "VM result relay passed\n"; return 0;
  } catch (const std::exception& e) { close(pair[0]); close(pair[1]); std::cerr << e.what() << '\n'; return 1; }
}
