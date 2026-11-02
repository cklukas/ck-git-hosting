// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "../../src/ci-vm-guest-macos/result_sink.hpp"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
  const char* tmp = std::getenv("TMPDIR");
  if (!tmp) return 1;
  std::string pattern = (std::filesystem::path(tmp) / "vm-guest-sink-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
  if (!mkdtemp(name.data())) return 1;
  const std::filesystem::path root(name.data());
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
  try {
    ckgit::CiPreparedJob job;
    ckgit::CiJob workflow_job;
    workflow_job.artifact = ckgit::CiArtifact{"output", {"build"}, 7};
    job.workflow.jobs.push_back(workflow_job);
    ckgit::CiRemotePolicy policy;
    ckgit::CiVmChannel guest(pair[0]), host(pair[1]);
    std::exception_ptr host_error;
    std::jthread server([&] {
      try {
        auto receive = [&] { return host.receive(std::chrono::seconds(5)); };
        auto ok = [&] { host.send(ckgit::CiVmFrameType::Event, "ok", std::chrono::seconds(5)); };
        require(receive().payload == "started 1", "start event missing"); ok();
        require(receive().payload == "step-start 0", "step start missing"); ok();
        require(receive().payload == "log 0", "log header missing");
        require(receive().payload == "hello", "log data missing"); ok();
        require(receive().payload.starts_with("step-end 0 0 0 0 "), "step end missing"); ok();
        require(receive().payload == "artifact 0 4 -", "artifact header missing");
        require(receive().payload == "data", "artifact body missing");
        require(receive().type == ckgit::CiVmFrameType::Done, "artifact end missing"); ok();
        const auto finish = receive();
        require(finish.type == ckgit::CiVmFrameType::Done &&
                    ckgit::parseCiFinishReport(finish.payload).outputs.size() == 1, "terminal report missing");
        ok();
      } catch (...) { host_error = std::current_exception(); shutdown(pair[1], SHUT_RDWR); }
    });
    ckgit::CiVmGuestSink sink(guest, job, policy, root);
    ckgit::CiRunRecord record; record.started_epoch_seconds = 1;
    sink.runStarted(record); sink.stepStarted(record, 0); sink.appendLog(0, "hello");
    record.steps.push_back({"build", 0, false, false}); sink.stepFinished(record, 0);
    std::ofstream(root / "bundle") << "data";
    require(sink.putArtifact(*job.workflow.jobs[0].artifact, root / "bundle", {}, false).bytes == 4,
            "artifact result incorrect");
    record.status = ckgit::CiRunStatus::Success;
    sink.finish(record);
    server.join();
    if (host_error) std::rethrow_exception(host_error);
    close(pair[0]); close(pair[1]);
    std::cout << "VM guest result relay passed\n";
    return 0;
  } catch (const std::exception& e) { close(pair[0]); close(pair[1]); std::cerr << e.what() << '\n'; return 1; }
}
