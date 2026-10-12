// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Unit tests for the pieces that let several CI runners share one server's
// queue: spool notifications, leases and requeueing, claim scheduling, and the
// runner protocol.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ckgit/ci_store.hpp"
#include "ckgit/dir_watch.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci distributed: " + message);
}

class Fixture {
 public:
  Fixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    auto pattern = (std::filesystem::path(configured_tmp) / "ckgit-ci-dist-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create isolated fixture");
    root = writable.data();
    state = root / "state";
    std::filesystem::create_directories(state);
    require(chmod(state.c_str(), 0700) == 0, "could not secure fixture state");
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path root;
  std::filesystem::path state;
};

ckgit::CiJobRequest sampleJob(const std::string& id, const std::string& ref = "refs/heads/main") {
  ckgit::CiJobRequest job;
  job.job_id = id;
  job.project_name = "demo";
  job.ref = ref;
  job.commit_id = std::string(40, 'a');
  job.client_id = "mac-studio";
  job.queued_epoch_seconds = 1700000000;
  return job;
}

// R0: a job queued by the hook wakes a waiting runner at once, without the
// runner reading the spool on a timer.
void testDirectoryWatchSeesQueuedJob() {
  Fixture fixture;
  const ckgit::CiQueuePaths queue = ckgit::ensureCiQueueDirectories(fixture.state);
  require(std::filesystem::is_directory(queue.spool) && std::filesystem::is_directory(queue.working),
          "the queue directories are created");
  ckgit::DirectoryWatch watch;
  require(watch.add(queue.spool), "the spool can be watched");
  require(watch.active(), "the watch is active on this platform");
  require(!watch.wait(std::chrono::milliseconds(50)), "an idle spool does not wake the watcher");

  std::thread producer([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ckgit::enqueueCiJob(fixture.state, sampleJob(ckgit::generateCiId()));
  });
  const auto start = std::chrono::steady_clock::now();
  const bool woke = watch.wait(std::chrono::seconds(10));
  const auto waited = std::chrono::steady_clock::now() - start;
  producer.join();
  require(woke, "queueing a job wakes the watcher");
  require(waited < std::chrono::seconds(5), "the watcher wakes promptly, not at its timeout");
  // Whatever is left of the burst one queued job causes (staging file, rename)
  // is consumed by the next wait; after that the watcher is idle again.
  watch.wait(std::chrono::milliseconds(100));
  require(!watch.wait(std::chrono::milliseconds(50)), "the watcher is idle once the burst is consumed");
}

void testDirectoryWatchInactiveSleeps() {
  ckgit::DirectoryWatch watch;
  require(!watch.add("/nonexistent/ckgit-dir-watch-probe"), "a missing directory cannot be watched");
  require(!watch.active(), "a watch without directories is inactive");
  const auto start = std::chrono::steady_clock::now();
  require(!watch.wait(std::chrono::milliseconds(30)), "an inactive watch reports a timeout");
  require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25),
          "an inactive watch sleeps for its timeout");
}

}  // namespace

void testCiDistributed() {
  testDirectoryWatchSeesQueuedJob();
  testDirectoryWatchInactiveSleeps();
}
