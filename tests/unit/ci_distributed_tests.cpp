// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Unit tests for the pieces that let several CI runners share one server's
// queue: spool notifications, leases and requeueing, claim scheduling, and the
// runner protocol.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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


std::uint64_t epochNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// R1: a claim records a lease that only its holder can renew; releasing the
// job drops the lease with it.
void testLeaseClaimRenewRelease() {
  Fixture fixture;
  const std::string id = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(id));
  ckgit::CiClaimOptions options;
  options.runner = "uconsole";
  options.lease_seconds = 60;
  const std::optional<ckgit::CiClaim> claim = ckgit::claimCiJob(fixture.state, options);
  require(claim.has_value() && claim->job.job_id == id, "the queued job is claimed");
  require(claim->job.attempt == 1, "a fresh job is on its first attempt");
  require(claim->lease.runner == "uconsole" && claim->lease.token.size() == 32, "the claim carries a lease");
  require(claim->lease.expires_epoch_seconds >= epochNow() + 55, "the lease lasts its configured length");
  const auto loaded = ckgit::loadCiClaim(fixture.state, id);
  require(loaded.has_value() && loaded->lease.token == claim->lease.token, "the claim can be looked up");
  require(ckgit::renewCiLease(fixture.state, id, claim->lease.token, 60), "the holder renews its lease");
  require(!ckgit::renewCiLease(fixture.state, id, std::string(32, '0'), 60), "another token cannot renew it");
  require(!ckgit::claimCiJob(fixture.state, options).has_value(), "a claimed job is not handed out twice");
  ckgit::releaseCiJob(fixture.state, id);
  require(!ckgit::loadCiClaim(fixture.state, id).has_value(), "a released job is no longer claimed");
  require(!ckgit::renewCiLease(fixture.state, id, claim->lease.token, 60), "a released lease cannot be renewed");
  require(!std::filesystem::exists(fixture.state / "ci" / "leases" / (id + ".ini")), "the lease file is gone");
}

// R1: a lapsed lease puts the job back into the spool on its next attempt,
// with the lost attempt's logs kept aside and the run shown as Pending; after
// the last attempt the run is recorded as an error instead.
void testReaperRequeuesThenGivesUp() {
  Fixture fixture;
  const std::string id = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(id));
  ckgit::CiClaimOptions options;
  options.runner = "uconsole";
  options.lease_seconds = 30;
  const auto first = ckgit::claimCiJob(fixture.state, options);
  require(first.has_value(), "the job is claimed");
  const std::filesystem::path run_dir = fixture.state / "ci" / "runs" / "demo" / id;
  std::ofstream(run_dir / "steps" / "0.log") << "partial output\n";

  ckgit::CiReapOptions early;
  early.max_attempts = 2;
  const ckgit::CiReapResult none = ckgit::reapCiLeases(fixture.state, early);
  require(none.requeued.empty() && none.failed.empty(), "a live lease is left alone");
  require(none.next_expiry_epoch_seconds.has_value() &&
              *none.next_expiry_epoch_seconds == first->lease.expires_epoch_seconds,
          "the reaper reports when the next lease lapses");

  ckgit::CiReapOptions late = early;
  late.now_epoch_seconds = first->lease.expires_epoch_seconds + 1;
  const ckgit::CiReapResult reaped = ckgit::reapCiLeases(fixture.state, late);
  require(reaped.requeued.size() == 1 && reaped.requeued[0] == id, "a lapsed lease is requeued");
  require(!ckgit::loadCiClaim(fixture.state, id).has_value(), "the requeued job is no longer claimed");
  require(std::filesystem::exists(run_dir / "attempt-1" / "0.log"), "the lost attempt's log is kept aside");
  require(!std::filesystem::exists(run_dir / "steps" / "0.log"), "the new attempt starts with no logs");
  const auto pending = ckgit::loadCiRun(fixture.state, "demo", id);
  require(pending.has_value() && pending->status == ckgit::CiRunStatus::Pending &&
              pending->detail.find("uconsole") != std::string::npos,
          "the run is Pending again and names the lost runner");

  const auto second = ckgit::claimCiJob(fixture.state, options);
  require(second.has_value() && second->job.job_id == id && second->job.attempt == 2,
          "the requeued job is claimed again on its second attempt");
  late.now_epoch_seconds = second->lease.expires_epoch_seconds + 1;
  const ckgit::CiReapResult given_up = ckgit::reapCiLeases(fixture.state, late);
  require(given_up.failed.size() == 1 && given_up.requeued.empty(), "the last attempt is not requeued");
  const auto failed = ckgit::loadCiRun(fixture.state, "demo", id);
  require(failed.has_value() && failed->status == ckgit::CiRunStatus::Error, "the run is recorded as an error");
  require(!ckgit::claimCiJob(fixture.state, options).has_value(), "nothing is left to claim");
}

// R1: a restarted runner's own leases lapse at once, and a runner shutting
// down can hand its job back without waiting for the lease.
void testRestartAndAbandon() {
  Fixture fixture;
  const std::string first_id = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(first_id));
  const auto claim = ckgit::claimCiJob(fixture.state, ckgit::CiClaimOptions{});
  require(claim.has_value() && claim->lease.runner == ckgit::kCiPrimaryRunnerName,
          "the default claim belongs to the primary runner");
  ckgit::CiReapOptions restart;
  restart.restarted_runner = ckgit::kCiPrimaryRunnerName;
  const auto reaped = ckgit::reapCiLeases(fixture.state, restart);
  require(reaped.requeued.size() == 1, "a restarted runner's job is requeued without waiting");

  const auto again = ckgit::claimCiJob(fixture.state, ckgit::CiClaimOptions{});
  require(again.has_value() && again->job.attempt == 2, "the job is claimed again");
  require(!ckgit::abandonCiJob(fixture.state, first_id, std::string(32, 'f'), "primary"),
          "only the lease holder can give the job back");
  require(ckgit::abandonCiJob(fixture.state, first_id, again->lease.token, "primary"),
          "the holder gives the job back");
  const auto third = ckgit::claimCiJob(fixture.state, ckgit::CiClaimOptions{});
  require(third.has_value() && third->job.attempt == 3,
          "a job given back is requeued even past the attempt limit");
}

// R1: a job queued by an earlier build (schema 1, no attempt line) still runs.
void testVersionOneJobStillClaims() {
  Fixture fixture;
  ckgit::ensureCiQueueDirectories(fixture.state);
  const std::string id = ckgit::generateCiId();
  const std::filesystem::path file = fixture.state / "ci" / "spool" / (id + ".ini");
  {
    std::ofstream out(file);
    out << "schema_version=1\njob_id=" << id << "\nproject=demo\nref_hex=726566732f68656164732f6d61696e\n"
        << "commit=" << std::string(40, 'b') << "\nclient_id=mac\nqueued_epoch=1700000000\n";
  }
  require(chmod(file.c_str(), 0600) == 0, "could not secure the legacy job");
  const auto claim = ckgit::claimCiJob(fixture.state, ckgit::CiClaimOptions{});
  require(claim.has_value() && claim->job.attempt == 1 && claim->job.ref == "refs/heads/main",
          "a version-1 job is claimed as a first attempt");
}

}  // namespace

void testCiDistributed() {
  testDirectoryWatchSeesQueuedJob();
  testDirectoryWatchInactiveSleeps();
  testLeaseClaimRenewRelease();
  testReaperRequeuesThenGivesUp();
  testRestartAndAbandon();
  testVersionOneJobStillClaims();
}
