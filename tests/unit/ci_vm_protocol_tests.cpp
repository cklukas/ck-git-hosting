// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_vm_protocol.hpp"

#include <stdexcept>
#include <string>
#include <exception>
#ifndef _WIN32
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include "ckgit/ci_vm_source.hpp"
#include "ckgit/macos_vm_workspace.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class F> bool rejects(F fn) { try { fn(); return false; } catch (const std::exception&) { return true; } }
}
void testCiVmProtocol() {
  using namespace ckgit;
  CiPreparedJob prepared;
  prepared.job.job_id = "00000000000000000001-abcdabcd";
  prepared.job.project_name = "project";
  prepared.job.commit_id = std::string(40, 'a');
  prepared.job.ref = "refs/heads/main";
  prepared.workflow_text = "version: 1\njobs:\n  - name: build\n    steps:\n      - run: [true]\n";
  prepared.workflow = parseCiWorkflow(prepared.workflow_text);
  CiRemotePolicy host_policy;
  host_policy.allow_network = true;
  const auto assignment = formatCiVmAssignment(prepared, host_policy);
  const auto parsed_assignment = parseCiVmAssignment(assignment);
  require(parsed_assignment.prepared.job.commit_id == prepared.job.commit_id && !parsed_assignment.policy.allow_network &&
              parsed_assignment.policy.lease_seconds == 1,
          "VM assignment did not preserve identity or disable network and host lease");
  auto tampered_assignment = assignment;
  const auto token = tampered_assignment.find("token=" + std::string(32, '0'));
  require(token != std::string::npos, "VM assignment token fixture missing");
  tampered_assignment[token + 6] = '1';
  require(rejects([&] { (void)parseCiVmAssignment(tampered_assignment); }), "usable guest lease token accepted");
  tampered_assignment = assignment;
  const auto network = tampered_assignment.find("allow_network=0");
  require(network != std::string::npos, "VM assignment network fixture missing");
  tampered_assignment[network + std::string("allow_network=").size()] = '1';
  require(rejects([&] { (void)parseCiVmAssignment(tampered_assignment); }), "network-enabled guest assignment accepted");
  CiCheckoutRequest checkout{prepared.job.project_name, prepared.job.commit_id, prepared.workflow.checkout,
                             prepared.job.ref};
  require(ciVmCheckoutAllowed(prepared, checkout), "assigned VM checkout was rejected");
  auto changed = checkout;
  changed.commit = std::string(40, 'b');
  require(!ciVmCheckoutAllowed(prepared, changed), "VM checkout accepted another commit");
  changed = checkout;
  changed.spec.depth = 0;
  require(!ciVmCheckoutAllowed(prepared, changed), "VM checkout accepted more history");
  changed = checkout;
  changed.ref = "refs/heads/other";
  require(!ciVmCheckoutAllowed(prepared, changed), "VM checkout accepted another ref");
  CiResolvedSister sister{"sister", std::string(40, 'c'), {}};
  prepared.sisters.push_back(sister);
  CiCheckoutRequest sister_checkout{sister.name, sister.commit, sister.checkout, {}};
  require(ciVmCheckoutAllowed(prepared, sister_checkout), "assigned sister checkout was rejected");
  sister_checkout.project = "another-project";
  require(!ciVmCheckoutAllowed(prepared, sister_checkout), "VM checkout accepted an undeclared project");
  require(parseCiVmCheckoutRequest(formatCiVmCheckoutRequest(0), prepared).commit == prepared.job.commit_id,
          "VM primary checkout ordinal did not resolve to the host assignment");
  require(parseCiVmCheckoutRequest(formatCiVmCheckoutRequest(1), prepared).commit == sister.commit,
          "VM sister checkout ordinal did not resolve to the host assignment");
  for (const auto& invalid : {"checkout 2", "checkout 01", "checkout -1", "checkout 1 extra",
                              "checkout 184467440737095516160000", "checkout ", "other 0"})
    require(rejects([&] { (void)parseCiVmCheckoutRequest(invalid, prepared); }),
            "VM accepted a malformed or unassigned checkout ordinal");
  prepared.workflow.jobs[0].artifact = CiArtifact{"declared", {"out"}, 7};
  CiRemotePolicy bounded_policy;
  bounded_policy.max_log_bytes = 4;
  bounded_policy.artifact_max_bytes = 5;
  CiVmOutputGuard outputs(prepared, bounded_policy);
  outputs.log(0, 3);
  require(rejects([&] { outputs.log(0, 2); }) && rejects([&] { outputs.log(1, 1); }),
          "VM accepted an oversized log or undeclared step");
  require(rejects([&] { (void)outputs.beginArtifact(1, 1); }) &&
              rejects([&] { (void)outputs.beginArtifact(0, 6); }),
          "VM accepted an undeclared or oversized artifact");
  const auto& artifact = outputs.beginArtifact(0, 5);
  require(artifact.name == "declared" && artifact.retention_days == 7,
          "VM artifact was not resolved against host assignment");
  outputs.artifactChunk(3);
  require(rejects([&] { outputs.endArtifact(); }) && rejects([&] { outputs.artifactChunk(3); }),
          "VM accepted a truncated or overlong artifact");
  outputs.artifactChunk(2);
  outputs.endArtifact();
  require(rejects([&] { (void)outputs.beginArtifact(0, 0); }) &&
              rejects([&] { outputs.artifactChunk(1); }), "VM accepted a duplicate or out-of-order artifact");
  const std::string binary("a\0b\xff", 4);
  const auto first = encodeCiVmFrame(CiVmFrameType::Chunk, binary);
  const auto second = encodeCiVmFrame(CiVmFrameType::Cancel, {});
  CiVmFrameDecoder incremental;
  for (std::size_t i = 0; i + 1 < first.size(); ++i) {
    incremental.feed(std::string_view(first).substr(i, 1));
    require(!incremental.next(), "partial VM frame was accepted");
  }
  incremental.feed(std::string_view(first).substr(first.size() - 1));
  auto decoded = incremental.next();
  require(decoded && decoded->type == CiVmFrameType::Chunk && decoded->payload == binary,
          "fragmented binary VM frame was corrupted");
  incremental.feed(first + second);
  require(incremental.next()->payload == binary && incremental.next()->type == CiVmFrameType::Cancel &&
              !incremental.next(), "coalesced VM frames were not decoded in order");
  incremental.finish();
  CiVmFrameDecoder truncated;
  truncated.feed(std::string_view(first).substr(0, first.size() - 1));
  require(!truncated.next() && rejects([&] { truncated.finish(); }) && truncated.failed(),
          "truncated VM frame at EOF was accepted");
  require(rejects([] { encodeCiVmFrame(CiVmFrameType::Chunk, std::string(kCiVmChunkPayloadMax + 1, 'x')); }),
          "oversized VM data frame was encoded");
  require(rejects([] { encodeCiVmFrame(static_cast<CiVmFrameType>(0), {}); }), "unknown VM type was encoded");
  for (const auto& bad : {std::string("BAD1\1\0\0\0\0", 9), std::string("CKV1\xff\0\0\0\0", 9),
                          std::string("CKV1\5\xff\xff\xff\xff", 9)}) {
    CiVmFrameDecoder decoder;
    decoder.feed(bad);
    require(rejects([&] { (void)decoder.next(); }) && decoder.failed() &&
                rejects([&] { decoder.feed(first); }), "invalid VM frame did not poison the decoder");
  }
  CiVmFrameDecoder bounded;
  require(rejects([&] { bounded.feed(std::string(kCiVmInputBufferMax + 1, 'x')); }) && bounded.failed(),
          "VM input buffer was not bounded");
#ifndef _WIN32
  struct Pair {
    int fd[2] = {-1, -1};
    Pair() { require(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0, "VM socket pair creation failed"); }
    ~Pair() { for (int value : fd) if (value >= 0) close(value); }
  };
  {
    Pair pair;
    CiVmChannel host(pair.fd[0]), guest(pair.fd[1]);
    std::optional<CiVmAssignment> assigned;
    std::exception_ptr guest_error;
    std::jthread worker([&] {
      try { assigned = ciVmGuestHandshake(guest, std::chrono::seconds(2)); }
      catch (...) { guest_error = std::current_exception(); }
    });
    ciVmHostHandshake(host, parsed_assignment.prepared, host_policy, std::chrono::seconds(2));
    worker.join();
    if (guest_error) std::rethrow_exception(guest_error);
    require(assigned && assigned->prepared.job.commit_id == parsed_assignment.prepared.job.commit_id &&
                !assigned->policy.allow_network, "VM handshake did not deliver the restricted assignment");
  }
  {
    Pair pair;
    CiVmChannel host(pair.fd[0]), guest(pair.fd[1]);
    guest.send(CiVmFrameType::Hello, "wrong-version", std::chrono::seconds(1));
    require(rejects([&] { ciVmHostHandshake(host, parsed_assignment.prepared, host_policy, std::chrono::seconds(1)); }) &&
                host.failed(), "VM handshake accepted a wrong protocol or remained usable");
  }
  {
    Pair pair;
    CiVmChannel sender(pair.fd[0]), receiver(pair.fd[1]);
    const std::string payload(kCiVmChunkPayloadMax, 'z');
    std::exception_ptr send_error;
    std::jthread writer([&] {
      try { sender.send(CiVmFrameType::Chunk, payload, std::chrono::seconds(2)); }
      catch (...) { send_error = std::current_exception(); }
    });
    const auto frame = receiver.receive(std::chrono::seconds(2));
    writer.join();
    if (send_error) std::rethrow_exception(send_error);
    require(frame.type == CiVmFrameType::Chunk && frame.payload == payload,
            "VM socket lost a large fragmented frame");
  }
  {
    Pair pair;
    CiVmChannel receiver(pair.fd[1]);
    require(write(pair.fd[0], first.data(), first.size() - 1) == static_cast<ssize_t>(first.size() - 1),
            "VM truncated fixture write failed");
    shutdown(pair.fd[0], SHUT_WR);
    require(rejects([&] { (void)receiver.receive(std::chrono::seconds(2)); }) && receiver.failed(),
            "VM socket accepted truncated EOF");
  }
  {
    Pair pair;
    CiVmChannel receiver(pair.fd[1]);
    require(rejects([&] { (void)receiver.receive(std::chrono::seconds(1), [] { return false; }); }) && receiver.failed(),
            "cancelled VM socket stayed usable");
  }
  {
    Pair pair;
    CiVmChannel receiver(pair.fd[1]);
    const auto start = std::chrono::steady_clock::now();
    require(rejects([&] { (void)receiver.receive(std::chrono::milliseconds(20)); }) && receiver.failed() &&
                std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "idle VM socket did not enforce its deadline");
  }
  {
    Pair pair;
    CiVmChannel receiver(pair.fd[1]);
    require(rejects([&] { (void)receiver.receive(std::chrono::milliseconds::max()); }) && receiver.failed(),
            "VM socket accepted an overflowing deadline");
  }
  {
    Pair pair;
    CiVmChannel sender(pair.fd[0]);
    close(pair.fd[1]); pair.fd[1] = -1;
    require(rejects([&] { sender.send(CiVmFrameType::Hello, {}, std::chrono::seconds(1)); }) && sender.failed(),
            "closed VM socket did not fail safely");
  }
#endif
#ifdef __APPLE__
  {
    const char* temporary = std::getenv("TMPDIR");
    require(temporary && *temporary, "VM workspace test needs TMPDIR");
    std::string pattern = (std::filesystem::path(temporary) / "vm-use-lock-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "cannot create VM lock fixture");
    const std::filesystem::path root(writable.data());
    struct Cleanup {
      std::filesystem::path root;
      ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    std::filesystem::create_directory(root / "base");
    std::filesystem::create_directory(root / "work");
    require(chmod((root / "work").c_str(), 0700) == 0, "cannot protect VM lock fixture");
    for (const char* name : {"Disk.img", "AuxiliaryStorage", "MachineIdentifier", "HardwareModel"})
      std::ofstream(root / "base" / name) << "test image component";
    ckgit::MacosVmRunnerLock runner_lock(root / "work");
    require(rejects([&] { ckgit::MacosVmRunnerLock other(root / "work"); }),
            "a second VM runner acquired the same work root");
    ckgit::MacosVmWorkspace workspace(root / "base", root / "work", 1);
    struct FixtureSource : CiSourceProvider {
      std::vector<std::string> commits;
      void checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination) override {
        commits.push_back(request.commit);
        std::ofstream(destination / "fixture") << request.project;
      }
    } source;
    stageCiVmSources(prepared, source, workspace.exchange());
    require(source.commits.size() == 2 && source.commits[0] == prepared.job.commit_id &&
                source.commits[1] == sister.commit &&
                std::filesystem::exists(workspace.exchange() / "source" / "0" / "fixture") &&
                std::filesystem::exists(workspace.exchange() / "source" / "1" / "fixture"),
            "VM source staging did not use the host assignment in order");
    require(rejects([&] { stageCiVmSources(prepared, source, workspace.exchange()); }),
            "VM source staging accepted an existing checkout tree");
    const auto bundle = workspace.bundle();
    const int lock = open((bundle / ".ckgit-vm-use.lock").c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    require(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0, "cannot hold VM use lock");
    require(rejects([&] { workspace.removeAfterConfirmedStop(); }) && std::filesystem::exists(bundle / "Disk.img"),
            "live VM use lock did not prevent workspace removal");
    close(lock);
    workspace.removeAfterConfirmedStop();
    require(!std::filesystem::exists(bundle), "stopped VM workspace was not removed");
  }
#endif
}
