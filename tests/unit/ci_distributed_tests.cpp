// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Unit tests for the pieces that let several CI runners share one server's
// queue: spool notifications, leases and requeueing, claim scheduling, and the
// runner protocol.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fcntl.h>

#include "ckgit/authorized_keys.hpp"
#include "ckgit/ci_control.hpp"
#include "ckgit/ci_gateway.hpp"
#include "ckgit/ci_remote.hpp"
#include "ckgit/ci_results.hpp"
#include "ckgit/ci_runner_protocol.hpp"
#include "ckgit/process.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/ssh_command.hpp"
#include "ckgit/web_renderer.hpp"
#include "ckgit/text.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/pages_store.hpp"
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


bool throws(const std::function<void()>& action) {
  try {
    action();
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

// R2b: a staged upload is resumable and exactly-once -- a retried chunk is
// not stored twice, a gap is refused, and the announced size is capped before
// any byte is stored.
void testStagedUploadResume() {
  Fixture fixture;
  const std::string run = ckgit::generateCiId();
  const std::string data = "0123456789";
  const std::string sha = ckgit::sha256Hex(data);
  using ckgit::CiUploadKind;
  require(ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "bundle", 10, sha, 0,
                                      data.substr(0, 4), 100) == 4,
          "the first chunk is held");
  require(ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "bundle", 10, sha, 0,
                                      data.substr(0, 4), 100) == 4,
          "a retried chunk is not stored twice");
  require(throws([&]() {
            ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "bundle", 10, sha, 7,
                                        "789", 100);
          }),
          "a chunk that skips bytes is refused");
  require(ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "bundle", 10, sha, 4,
                                      data.substr(4), 100) == 10,
          "the upload resumes where the server's bytes end");
  require(throws([&]() {
            ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "huge", 1000, sha, 0,
                                        "x", 100);
          }),
          "an upload over the cap is refused before any byte is stored");
  require(throws([&]() {
            ckgit::appendCiStagedUpload(fixture.state, "demo", run, CiUploadKind::kArtifact, "release", 10, sha, 0,
                                        "x", 100);
          }),
          "the reserved name cannot be uploaded");
  const auto staged = ckgit::listCiStagedUploads(fixture.state, "demo", run);
  require(staged.size() == 1 && staged[0].name == "bundle" && staged[0].held == 10 && staged[0].total == 10,
          "the staged upload is listed complete");
}

ckgit::CiPreparedJob preparedJob(const std::string& run, const std::string& ref, bool default_branch,
                                 const std::string& workflow_text) {
  ckgit::CiPreparedJob prepared;
  prepared.job = sampleJob(run, ref);
  prepared.workflow_text = workflow_text;
  prepared.workflow = ckgit::parseCiWorkflow(workflow_text);
  prepared.is_default_branch = default_branch;
  if (ref.rfind("refs/tags/", 0) == 0) {
    prepared.is_release = true;
    prepared.release_tag = ref.substr(10);
  }
  return prepared;
}

ckgit::CiRunRecord finishedRecord(const ckgit::CiPreparedJob& prepared, ckgit::CiRunStatus status) {
  ckgit::CiRunRecord record;
  record.run_id = prepared.job.job_id;
  record.project_name = prepared.job.project_name;
  record.ref = prepared.job.ref;
  record.commit_id = prepared.job.commit_id;
  record.status = status;
  record.detail = "all steps passed";
  return record;
}

// R2b: committing verifies every staged output; an incomplete or corrupt one
// is recorded with a note instead of bytes, and a release only appears when the
// whole run succeeded.
void testCommitVerifiesStagedOutputs() {
  Fixture fixture;
  ckgit::CiCommitPolicy policy;
  policy.state_root = fixture.state;
  const std::string run = ckgit::generateCiId();
  const auto prepared = preparedJob(run, "refs/heads/main", true, "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  const std::string good = "good-bundle";
  ckgit::appendCiStagedUpload(fixture.state, "demo", run, ckgit::CiUploadKind::kArtifact, "good", good.size(),
                              ckgit::sha256Hex(good), 0, good, 1u << 20);
  ckgit::appendCiStagedUpload(fixture.state, "demo", run, ckgit::CiUploadKind::kArtifact, "half", 100,
                              ckgit::sha256Hex(std::string(100, 'x')), 0, "xxxx", 1u << 20);
  ckgit::appendCiStagedUpload(fixture.state, "demo", run, ckgit::CiUploadKind::kArtifact, "corrupt", 3,
                              ckgit::sha256Hex("abc"), 0, "abd", 1u << 20);
  const std::vector<ckgit::CiDeclaredOutput> outputs = {
      {"good", false, 0, ""}, {"half", false, 0, ""}, {"corrupt", false, 0, ""}, {"missing", false, 0, ""},
      {"noted", false, 0, "artifact exceeds the size cap"}};
  const ckgit::CiRunRecord stored =
      ckgit::commitCiRun(policy, prepared, finishedRecord(prepared, ckgit::CiRunStatus::Failure), outputs);
  require(stored.status == ckgit::CiRunStatus::Failure, "the run keeps its status");
  const auto run_loaded = ckgit::loadCiRun(fixture.state, "demo", run);
  require(run_loaded.has_value() && run_loaded->artifacts.size() == 5, "every declared artifact is listed");
  for (const ckgit::CiArtifactRecord& artifact : run_loaded->artifacts) {
    if (artifact.name == "good") {
      require(artifact.note.empty() && artifact.bytes == good.size(), "a verified artifact is stored");
    } else {
      require(!artifact.note.empty() && artifact.bytes == 0, "an unusable artifact carries a note: " + artifact.name);
    }
  }
  require(ckgit::readCiArtifact(fixture.state, "demo", run, "good", 1024) == good, "the stored bytes are the upload");
  require(ckgit::listCiStagedUploads(fixture.state, "demo", run).empty(), "the staging is discarded");

  const std::string tag_run = ckgit::generateCiId();
  const auto tag_job = preparedJob(tag_run, "refs/tags/v1", false, "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  const std::string asset = "asset-bytes";
  ckgit::appendCiStagedUpload(fixture.state, "demo", tag_run, ckgit::CiUploadKind::kAsset, "pkg", asset.size(),
                              ckgit::sha256Hex(asset), 0, asset, 1u << 20);
  ckgit::commitCiRun(policy, tag_job, finishedRecord(tag_job, ckgit::CiRunStatus::Failure), {{"pkg", true, 0, ""}});
  require(ckgit::loadReleases(fixture.state, "demo").empty(), "a failed tag build publishes no release");
  ckgit::appendCiStagedUpload(fixture.state, "demo", tag_run, ckgit::CiUploadKind::kAsset, "pkg", asset.size(),
                              ckgit::sha256Hex(asset), 0, asset, 1u << 20);
  ckgit::commitCiRun(policy, tag_job, finishedRecord(tag_job, ckgit::CiRunStatus::Success), {{"pkg", true, 0, ""}});
  const auto releases = ckgit::loadReleases(fixture.state, "demo");
  require(releases.size() == 1 && releases[0].assets.size() == 1 && releases[0].assets[0].bytes == asset.size(),
          "a successful tag build publishes its verified asset");
}

// R2b: an older run never replaces a newer run's live Pages site, and a site
// can arrive as a staged tar (the remote path).
void testPagesOrderingGuard() {
  Fixture fixture;
  ckgit::CiCommitPolicy policy;
  policy.state_root = fixture.state;
  policy.pages_root = fixture.root / "pages";
  const std::string text = "version: 1\npages: { path: public }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n";
  const std::string older = ckgit::generateCiId();
  const std::string newer = ckgit::generateCiId();
  const std::filesystem::path site = fixture.root / "site";
  std::filesystem::create_directories(site);
  std::ofstream(site / "index.html") << "newer";
  const auto newer_job = preparedJob(newer, "refs/heads/main", true, text);
  ckgit::commitCiRun(policy, newer_job, finishedRecord(newer_job, ckgit::CiRunStatus::Success), {}, site);
  require(ckgit::currentPagesVersion(policy.pages_root, "demo") == newer, "the newer run's site is live");

  // The older run's site travels as a tar, as a remote runner would send it.
  const std::filesystem::path old_tree = fixture.root / "old";
  std::filesystem::create_directories(old_tree);
  std::ofstream(old_tree / "index.html") << "older";
  const std::filesystem::path tar = fixture.root / "old.tar";
  require(std::system(("tar -cf '" + tar.string() + "' -C '" + old_tree.string() + "' .").c_str()) == 0,
          "could not pack the test site");
  std::ifstream in(tar, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ckgit::appendCiStagedUpload(fixture.state, "demo", older, ckgit::CiUploadKind::kPages, ckgit::kCiPagesUploadName,
                              bytes.size(), ckgit::sha256Hex(bytes), 0, bytes, 1u << 20);
  const auto older_job = preparedJob(older, "refs/heads/main", true, text);
  const ckgit::CiRunRecord kept =
      ckgit::commitCiRun(policy, older_job, finishedRecord(older_job, ckgit::CiRunStatus::Success), {});
  require(kept.status == ckgit::CiRunStatus::Success, "the older run still succeeds");
  require(ckgit::currentPagesVersion(policy.pages_root, "demo") == newer, "the older run does not roll the site back");
  const auto page = ckgit::readCurrentPage(policy.pages_root, "demo", "", 1024);
  require(page.has_value() && page->content == "newer", "the live site is still the newer one");

  // A newer run's staged tar is unpacked and published.
  const std::string newest = ckgit::generateCiId();
  ckgit::appendCiStagedUpload(fixture.state, "demo", newest, ckgit::CiUploadKind::kPages, ckgit::kCiPagesUploadName,
                              bytes.size(), ckgit::sha256Hex(bytes), 0, bytes, 1u << 20);
  const auto newest_job = preparedJob(newest, "refs/heads/main", true, text);
  ckgit::commitCiRun(policy, newest_job, finishedRecord(newest_job, ckgit::CiRunStatus::Success), {});
  const auto replaced = ckgit::readCurrentPage(policy.pages_root, "demo", "", 1024);
  require(replaced.has_value() && replaced->content == "older", "a staged site tar is unpacked and published");
}


// R3: the primary's claim delay holds a fresh job back (so a free secondary
// gets it first) and reports when it becomes eligible; a secondary claims at
// once.
void testClaimDelay() {
  Fixture fixture;
  const std::string id = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(id));
  ckgit::CiClaimOptions primary;
  primary.min_age = std::chrono::seconds(60);
  std::optional<std::chrono::milliseconds> retry;
  require(!ckgit::claimCiJob(fixture.state, primary, &retry).has_value(), "the primary leaves a fresh job alone");
  require(retry.has_value() && *retry > std::chrono::seconds(50) && *retry <= std::chrono::seconds(60),
          "the primary learns when the job becomes eligible");
  ckgit::CiClaimOptions secondary;
  secondary.runner = "uconsole";
  const auto claim = ckgit::claimCiJob(fixture.state, secondary, &retry);
  require(claim.has_value() && claim->job.job_id == id, "a secondary claims the fresh job at once");
  require(!retry.has_value(), "nothing is left waiting on the delay");

  const std::string later = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(later, "refs/heads/other"));
  primary.min_age = std::chrono::milliseconds(150);
  require(!ckgit::claimCiJob(fixture.state, primary).has_value(), "inside the delay the job waits");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto taken = ckgit::claimCiJob(fixture.state, primary);
  require(taken.has_value() && taken->job.job_id == later, "after the delay the primary claims it");
}

// R3: a project's runs on one ref never overlap; other refs still run.
void testOneRunPerRef() {
  Fixture fixture;
  const std::string first = ckgit::generateCiId();
  const std::string second = ckgit::generateCiId();
  const std::string other = ckgit::generateCiId();
  ckgit::enqueueCiJob(fixture.state, sampleJob(first));
  ckgit::enqueueCiJob(fixture.state, sampleJob(second));
  ckgit::enqueueCiJob(fixture.state, sampleJob(other, "refs/heads/feature"));
  ckgit::CiClaimOptions runner_a;
  runner_a.runner = "runner-a";
  ckgit::CiClaimOptions runner_b;
  runner_b.runner = "runner-b";
  const auto a = ckgit::claimCiJob(fixture.state, runner_a);
  require(a.has_value() && a->job.job_id == first, "the oldest job is claimed first");
  const auto b = ckgit::claimCiJob(fixture.state, runner_b);
  require(b.has_value() && b->job.job_id == other, "the next free runner skips the busy ref and takes another");
  require(!ckgit::claimCiJob(fixture.state, runner_b).has_value(), "the second job on the busy ref waits");
  ckgit::releaseCiJob(fixture.state, first);
  const auto c = ckgit::claimCiJob(fixture.state, runner_b);
  require(c.has_value() && c->job.job_id == second, "once the ref is free its next job runs");
}


// R4: the runner protocol's command grammar.
void testRunnerCommandGrammar() {
  const std::string id = "00000000000000000001-aaaaaaaa";
  const std::string token(32, 'a');
  const std::string sha(64, 'b');
  const auto parse = [](const std::string& line) { return ckgit::parseCiRunnerCommand(line); };
  require(parse("ckgit-runner 1 claim 60").has_value(), "claim parses");
  require(parse("ckgit-runner 1 hello 302e362e30").has_value(), "hello parses");
  require(parse("ckgit-runner 1 stream " + id + " " + token).has_value(), "stream parses");
  require(parse("ckgit-runner 1 upload-pack " + id + " " + token + " libx").has_value(), "upload-pack parses");
  const auto put = parse("ckgit-runner 1 put " + id + " " + token + " artifact bundle 0 10 " + sha);
  require(put.has_value() && put->total == 10 && put->kind == "artifact", "put parses");
  require(ckgit::formatCiRunnerCommand(*put) == "ckgit-runner 1 put " + id + " " + token + " artifact bundle 0 10 " + sha,
          "a command formats back to its line");
  require(!parse("ckgit-runner 2 claim 60").has_value(), "another protocol version is refused");
  require(!parse("ckgit-runner 1 claim 600").has_value(), "a claim wait over the bound is refused");
  require(!parse("ckgit-runner 1 claim 060").has_value(), "a non-canonical number is refused");
  require(!parse("ckgit-runner 1 stream " + id + " short").has_value(), "a malformed token is refused");
  require(!parse("ckgit-runner 1 put " + id + " " + token + " artifact bundle 11 10 " + sha).has_value(),
          "an offset past the total is refused");
  require(!parse("ckgit-runner 1 put " + id + " " + token + " binary bundle 0 10 " + sha).has_value(),
          "an unknown upload kind is refused");
  require(!parse("ckgit-runner 1 upload-pack " + id + " " + token + " ../evil").has_value(),
          "an unsafe project is refused");
  require(!parse("ckgit-rpc 1 ping").has_value(), "a device operation is not a runner operation");
  require(!parse("ckgit-runner 1 claim 60; rm -rf /").has_value(), "shell syntax is refused");
  require(!ckgit::parseSshOriginalCommand("ckgit-runner 1 claim 60").has_value(),
          "a device key cannot run runner operations");
}

// R4: what the gateway sends and the runner reads back are one definition.
void testProtocolRoundTrips() {
  ckgit::CiPreparedJob prepared;
  prepared.job = sampleJob(ckgit::generateCiId(), "refs/tags/v1.2");
  prepared.job.attempt = 2;
  prepared.workflow_text =
      "version: 1\nsisters:\n  - name: libx\n    depth: 7\njobs:\n  - name: a\n    steps:\n      - run: [x]\n";
  prepared.workflow = ckgit::parseCiWorkflow(prepared.workflow_text);
  prepared.is_release = true;
  prepared.release_tag = "v1.2";
  prepared.sisters.push_back(ckgit::CiResolvedSister{"libx", std::string(40, 'c'), {}});
  ckgit::CiLease lease;
  lease.token = std::string(32, 'd');
  ckgit::CiRemotePolicy policy;
  policy.timeout_seconds = 77;
  policy.allow_network = true;
  const auto remote = ckgit::parseCiClaimResponse(ckgit::formatCiClaimResponse(prepared, lease, policy));
  require(remote.has_value() && remote->prepared.job.job_id == prepared.job.job_id &&
              remote->prepared.job.ref == "refs/tags/v1.2" && remote->prepared.job.attempt == 2,
          "the job survives the round trip");
  require(remote->prepared.is_release && remote->prepared.release_tag == "v1.2" && remote->token == lease.token,
          "release and lease survive the round trip");
  require(remote->policy.timeout_seconds == 77 && remote->policy.allow_network, "the server's policy travels along");
  require(remote->prepared.sisters.size() == 1 && remote->prepared.sisters[0].checkout.depth == 7,
          "a sister's resolved commit and history settings arrive");
  require(!ckgit::parseCiClaimResponse("none\n").has_value(), "none means no job");

  ckgit::CiFinishReport report;
  report.status = ckgit::CiRunStatus::Timeout;
  report.started_epoch_seconds = 1234;
  report.detail = "step 'x' exceeded its 1s budget";
  report.steps.push_back(ckgit::CiStepResult{"build", -1, true, false});
  report.steps.push_back(ckgit::CiStepResult{"", 3, false, true});
  report.outputs.push_back(ckgit::CiDeclaredOutput{"pkg", true, 5, ""});
  report.outputs.push_back(ckgit::CiDeclaredOutput{"big", false, 0, "artifact exceeds the size cap"});
  const ckgit::CiFinishReport parsed = ckgit::parseCiFinishReport(ckgit::formatCiFinishReport(report));
  require(parsed.status == report.status && parsed.detail == report.detail && parsed.started_epoch_seconds == 1234,
          "the final status round-trips");
  require(parsed.steps.size() == 2 && parsed.steps[0].exit_code == -1 && parsed.steps[0].timed_out &&
              parsed.steps[1].name.empty() && parsed.steps[1].output_truncated,
          "the steps round-trip");
  require(parsed.outputs.size() == 2 && parsed.outputs[0].release && parsed.outputs[0].retention_days == 5 &&
              parsed.outputs[1].note == "artifact exceeds the size cap",
          "the declared outputs round-trip");
}

// A bare repository demo.git under the fixture's repo root with one commit
// carrying `workflow`; returns the commit id.
std::string hostProject(const Fixture& fixture, const std::string& workflow) {
  const std::filesystem::path bare = fixture.root / "repos" / "demo.git";
  const std::filesystem::path work = fixture.root / "work";
  const auto run = [](const std::vector<std::string>& command) {
    const auto result = ckgit::runProcess(command, std::chrono::seconds(30));
    if (result.exit_code != 0) throw std::runtime_error("fixture git failed: " + result.output);
    return result.output;
  };
  std::filesystem::create_directories(fixture.root / "repos");
  run({"git", "init", "-q", "--bare", "--initial-branch=main", bare.string()});
  run({"git", "init", "-q", "--initial-branch=main", work.string()});
  std::filesystem::create_directories(work / ".ckgit");
  std::ofstream(work / ".ckgit" / "ci.yml") << workflow;
  run({"git", "-C", work.string(), "add", "-A"});
  run({"git", "-C", work.string(), "-c", "user.email=t@example.invalid", "-c", "user.name=T", "commit", "-q", "-m",
       "ci"});
  run({"git", "-C", work.string(), "push", "-q", "file://" + bare.string(), "HEAD:main"});
  std::string id = run({"git", "-C", work.string(), "rev-parse", "HEAD"});
  while (!id.empty() && id.back() == '\n') id.pop_back();
  return id;
}

// Runs one gateway operation with `input` as its stdin; returns its stdout.
std::string gateway(const ckgit::ServerConfig& config, const std::string& runner, const std::string& line,
                    const std::string& input, const Fixture& fixture, int* code = nullptr) {
  const auto command = ckgit::parseCiRunnerCommand(line);
  require(command.has_value(), "test command parses: " + line);
  const std::filesystem::path in_path = fixture.root / "gateway.in";
  const std::filesystem::path out_path = fixture.root / "gateway.out";
  std::ofstream(in_path, std::ios::binary | std::ios::trunc) << input;
  const int in = ::open(in_path.c_str(), O_RDONLY);
  const int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  const int result = ckgit::runCiGateway(config, runner, *command, in, out);
  ::close(in);
  ::close(out);
  if (code != nullptr) *code = result;
  std::ifstream read(out_path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(read)), std::istreambuf_iterator<char>());
}

// R4: a whole remote run through the gateway -- claim, live progress through
// the stream, a resumable upload, and the commit at finish -- and the lease
// check that refuses anyone else.
void testGatewayRun() {
  Fixture fixture;
  const std::string commit = hostProject(fixture,
                                         "version: 1\njobs:\n  - name: build\n    steps:\n      - run: [true]\n"
                                         "    artifacts:\n      name: bundle\n      paths: [out]\n");
  ckgit::ServerConfig config;
  config.repo_root = fixture.root / "repos";
  config.state_root = fixture.state;
  ckgit::CiJobRequest job = sampleJob(ckgit::generateCiId());
  job.commit_id = commit;
  ckgit::enqueueCiJob(fixture.state, job);

  const std::string answer = gateway(config, "uconsole", "ckgit-runner 1 claim 0", "", fixture);
  const auto remote = ckgit::parseCiClaimResponse(answer);
  require(remote.has_value() && remote->prepared.job.job_id == job.job_id, "the runner claims the job");
  require(remote->prepared.is_default_branch, "main is the default branch of the hosted project");
  const std::string token = remote->token;
  const std::string tail = job.job_id + " " + token;

  int code = 0;
  const std::string stolen =
      gateway(config, "intruder", "ckgit-runner 1 heartbeat " + job.job_id + " " + token, "", fixture, &code);
  require(stolen == "lease-lost\n", "another runner cannot use the lease");
  require(gateway(config, "uconsole", "ckgit-runner 1 heartbeat " + tail, "", fixture) == "ok\n",
          "the holder's heartbeat renews the lease");
  const auto seen = ckgit::loadCiRunnerRecord(fixture.state, "uconsole");
  require(seen.has_value() && seen->last_seen_epoch_seconds >= epochNow() - 5,
          "a busy runner's heartbeat keeps its last contact current");

  const std::string frames = "started 1700000000\nstep-start 0 " + ckgit::hexEncode("build") +
                             "\nlog 0 0 6\nhello\nlog 0 3 7\nlo\nbye\nstep-end 0 0 0 0 " +
                             ckgit::hexEncode("build") + "\nheartbeat\n";
  const std::string relayed = gateway(config, "uconsole", "ckgit-runner 1 stream " + tail, frames, fixture);
  require(relayed.find("ready\n") != std::string::npos && relayed.find("ack 0 10\n") != std::string::npos,
          "the stream acknowledges what it holds (got: " + relayed + ")");
  const auto log = ckgit::readCiRunLog(fixture.state, "demo", job.job_id, 0, 1024);
  require(log.has_value() && *log == "hello\nbye\n", "overlapping log frames are stored exactly once");
  const auto running = ckgit::loadCiRun(fixture.state, "demo", job.job_id);
  require(running.has_value() && running->status == ckgit::CiRunStatus::Running && running->steps.size() == 1,
          "the stream keeps the run record current");
  const std::string again = gateway(config, "uconsole", "ckgit-runner 1 stream " + tail, "", fixture);
  require(again.rfind("held 0 10\nready\n", 0) == 0,
          "a reconnecting stream learns how much is held (got: " + again + ")");

  const std::string bundle = "bundle-bytes";
  const std::string sha = ckgit::sha256Hex(bundle);
  const std::string put1 = gateway(config, "uconsole",
                                   "ckgit-runner 1 put " + tail + " artifact bundle 0 " + std::to_string(bundle.size()) +
                                       " " + sha,
                                   bundle.substr(0, 5), fixture);
  require(put1 == "ok 5\n", "the first chunk is held");
  const std::string put2 = gateway(config, "uconsole",
                                   "ckgit-runner 1 put " + tail + " artifact bundle 5 " + std::to_string(bundle.size()) +
                                       " " + sha,
                                   bundle.substr(5), fixture);
  require(put2 == "ok " + std::to_string(bundle.size()) + "\n", "the upload completes");

  ckgit::CiFinishReport report;
  report.status = ckgit::CiRunStatus::Success;
  report.started_epoch_seconds = 1700000000;
  report.detail = "all steps passed";
  report.steps.push_back(ckgit::CiStepResult{"build", 0, false, false});
  report.outputs.push_back(ckgit::CiDeclaredOutput{"bundle", false, 0, ""});
  const std::string finished =
      gateway(config, "uconsole", "ckgit-runner 1 finish " + tail, ckgit::formatCiFinishReport(report), fixture);
  require(finished == "ok success\n", "the run commits (got: " + finished + ")");
  const auto done = ckgit::loadCiRun(fixture.state, "demo", job.job_id);
  require(done.has_value() && done->status == ckgit::CiRunStatus::Success && done->artifacts.size() == 1 &&
              done->artifacts[0].bytes == bundle.size(),
          "the finished run and its uploaded artifact are on the main host");
  require(!ckgit::loadCiClaim(fixture.state, job.job_id).has_value(), "the job is released");
  require(gateway(config, "uconsole", "ckgit-runner 1 heartbeat " + tail, "", fixture) == "lease-lost\n",
          "a finished job's lease is gone");
}

// R4: jobs that end without running never reach a runner.
void testGatewayFinishesSkippedJobs() {
  Fixture fixture;
  const std::string commit = hostProject(fixture,
                                         "version: 1\non: { branches: [release] }\njobs:\n  - name: a\n    steps:\n"
                                         "      - run: [true]\n");
  ckgit::ServerConfig config;
  config.repo_root = fixture.root / "repos";
  config.state_root = fixture.state;
  ckgit::CiJobRequest job = sampleJob(ckgit::generateCiId());
  job.commit_id = commit;
  ckgit::enqueueCiJob(fixture.state, job);
  require(gateway(config, "uconsole", "ckgit-runner 1 claim 0", "", fixture) == "none\n",
          "a job that is no trigger is not handed out");
  const auto skipped = ckgit::loadCiRun(fixture.state, "demo", job.job_id);
  require(skipped.has_value() && skipped->status == ckgit::CiRunStatus::Skipped,
          "it is recorded as skipped on the main host");
}


// R4: a runner key's forced command names the gateway's configuration; a
// device key's never does.
void testRunnerKeyLine() {
  const std::string key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOz6m3Iv7uX1rX5dJZp1fZ3v1nA0qkGqk1xQmvE7ZbBc runner@host";
  ckgit::ForcedCommandLayout layout{"/usr/bin/ck-git-shell", "/srv/ck-git-hosting/repos",
                                    "/run/ck-git-hosting/control.sock", "/var/lib/ck-git-hosting/state"};
  const std::string device = ckgit::renderAuthorizedKeyLine("mac", key, layout);
  require(device.find("--runner-config") == std::string::npos, "a device key has no runner role");
  layout.runner_config = "/etc/ck-git-hosting/server.ini";
  const std::string runner = ckgit::renderAuthorizedKeyLine("uconsole", key, layout);
  require(runner.find("--client-id uconsole") != std::string::npos &&
              runner.find("--runner-config /etc/ck-git-hosting/server.ini\"") != std::string::npos &&
              runner.rfind("restrict,command=", 0) == 0,
          "a runner key's forced command carries the runner role");
}


// R5: runner.ini is strict like server.ini.
void testRunnerConfig() {
  Fixture fixture;
  const std::filesystem::path file = fixture.root / "runner.ini";
  const auto write = [&](const std::string& text) { std::ofstream(file, std::ios::trunc) << text; };
  write("schema_version=1\nserver=ckgit@rpi4\nname=uconsole\nci_build_root=/var/lib/ck-git-runner/ci-build\n"
        "identity=/var/lib/ck-git-runner/.ssh/id_ed25519\nci_allow_network=false\nci_timeout_seconds=600\n");
  const ckgit::RunnerConfig config = ckgit::loadRunnerConfig(file);
  require(config.server == "ckgit@rpi4" && config.name == "uconsole" && !config.allow_network &&
              config.timeout_seconds == 600u && config.claim_wait_seconds == 60 && !config.cache_root.has_value(),
          "a runner configuration parses with clean-runner defaults");
  require(ckgit::loadRunnerConfig(file).build_root == "/var/lib/ck-git-runner/ci-build", "the build root parses");
  const ckgit::CiRemoteTransport transport(config, file);
  const auto argv = transport.sshArguments();
  require(argv.back() == "ckgit@rpi4" && std::find(argv.begin(), argv.end(), "BatchMode=yes") != argv.end(),
          "the transport runs ssh non-interactively against the server");
  require(transport.fetchUrl("demo") == "ckgit@rpi4:demo.git", "a project is fetched from the server");
  const auto rejected = [&](const std::string& text) {
    write(text);
    return throws([&]() { ckgit::loadRunnerConfig(file); });
  };
  const std::string base = "schema_version=1\nserver=ckgit@rpi4\nci_build_root=/b\n";
  require(rejected(base + "name=primary\n"), "the primary's name is reserved");
  require(rejected(base + "name=a b\n"), "an invalid name is refused");
  require(rejected(base + "name=x\nbogus=1\n"), "an unknown key is refused");
  require(rejected(base + "name=x\nname=y\n"), "a duplicate key is refused");
  require(rejected("schema_version=1\nserver=ckgit@rpi4\nname=x\n"), "the build root is required");
  require(rejected(base + "name=x\nclaim_wait_seconds=500\n"), "a long poll over the bound is refused");
}


// R5L: the gateway's stream keeps the server's log cap, answers a gap with
// where its bytes end, refuses frames for steps it does not know, and ends
// the session with lease-lost once the job was requeued.
void testStreamEdges() {
  Fixture fixture;
  const std::string commit = hostProject(fixture, "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [true]\n");
  ckgit::ServerConfig config;
  config.repo_root = fixture.root / "repos";
  config.state_root = fixture.state;
  config.ci_max_log_bytes = 1024;
  ckgit::CiJobRequest job = sampleJob(ckgit::generateCiId());
  job.commit_id = commit;
  ckgit::enqueueCiJob(fixture.state, job);
  const auto remote = ckgit::parseCiClaimResponse(gateway(config, "uconsole", "ckgit-runner 1 claim 0", "", fixture));
  require(remote.has_value(), "the job is claimed");
  const std::string tail = job.job_id + " " + remote->token;

  const std::string big(1500, 'x');
  const std::string capped = gateway(config, "uconsole", "ckgit-runner 1 stream " + tail,
                                     "started 1\nstep-start 0 -\nlog 0 0 1500\n" + big + "log 0 2000 3\nabc", fixture);
  require(capped.find("ack 0 1500\n") != std::string::npos, "bytes past the cap are acknowledged, not resent");
  require(capped.find("held 0 1024\n") != std::string::npos, "a gap is answered with where the held bytes end");
  const auto log = ckgit::readCiRunLog(fixture.state, "demo", job.job_id, 0, 1u << 20);
  require(log.has_value() && log->size() == 1024, "the step log stops at the server's cap");

  int code = 0;
  const std::string unknown =
      gateway(config, "uconsole", "ckgit-runner 1 stream " + tail, "log 7 0 1\nz", fixture, &code);
  require(code == 1 && unknown.find("error") != std::string::npos, "a log for an unknown step ends the session");

  ckgit::CiReapOptions lapse;
  lapse.now_epoch_seconds = epochNow() + 100000;
  ckgit::reapCiLeases(fixture.state, lapse);
  require(gateway(config, "uconsole", "ckgit-runner 1 stream " + tail, "heartbeat\n", fixture) == "lease-lost\n",
          "a requeued job's stream is refused at once");
}


// R6: the runner registry, and a disabled (draining) runner claims nothing.
void testRunnerRegistry() {
  Fixture fixture;
  const std::string commit = hostProject(fixture, "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [true]\n");
  ckgit::ServerConfig config;
  config.repo_root = fixture.root / "repos";
  config.state_root = fixture.state;
  require(ckgit::loadCiRunnerRecords(fixture.state).empty(), "no runner is registered at first");
  ckgit::CiRunnerRecord record;
  record.name = "uconsole";
  record.enabled = false;
  ckgit::writeCiRunnerRecord(fixture.state, record);
  require(throws([&]() {
            ckgit::CiRunnerRecord primary;
            primary.name = ckgit::kCiPrimaryRunnerName;
            ckgit::writeCiRunnerRecord(fixture.state, primary);
          }),
          "the primary's name cannot be registered");
  ckgit::CiJobRequest job = sampleJob(ckgit::generateCiId());
  job.commit_id = commit;
  ckgit::enqueueCiJob(fixture.state, job);
  require(gateway(config, "uconsole", "ckgit-runner 1 claim 0", "", fixture) == "none\n",
          "a disabled runner claims nothing");
  require(gateway(config, "uconsole", "ckgit-runner 1 hello " + ckgit::hexEncode("0.6.0-test"), "", fixture)
              .rfind("ok\n", 0) == 0,
          "hello answers");
  const auto seen = ckgit::loadCiRunnerRecord(fixture.state, "uconsole");
  require(seen.has_value() && !seen->enabled && seen->version == "0.6.0-test" && seen->last_seen_epoch_seconds > 0,
          "hello records the runner's version and last contact without enabling it");
  record.enabled = true;
  ckgit::writeCiRunnerRecord(fixture.state, record);
  const auto claimed = ckgit::parseCiClaimResponse(gateway(config, "uconsole", "ckgit-runner 1 claim 0", "", fixture));
  require(claimed.has_value(), "an enabled runner claims the job");
  const auto claims = ckgit::listCiClaims(fixture.state);
  require(claims.size() == 1 && claims[0].lease.runner == "uconsole" && claims[0].job.job_id == job.job_id,
          "the registry shows what each runner is working on");
  require(ckgit::removeCiRunnerRecord(fixture.state, "uconsole") && !ckgit::removeCiRunnerRecord(fixture.state, "uconsole"),
          "a runner record can be removed once");
}


// R7: a run records which runner executes it and which attempt it is; the
// dashboard and `ckgit ci show` show both.
void testRunnerShownOnRuns() {
  Fixture fixture;
  ckgit::CiRunRecord run;
  run.run_id = ckgit::generateCiId();
  run.project_name = "demo";
  run.ref = "refs/heads/main";
  run.commit_id = std::string(40, 'a');
  run.status = ckgit::CiRunStatus::Running;
  run.runner = "uconsole";
  run.attempt = 2;
  ckgit::writeCiRunRecord(fixture.state, run);
  const auto loaded = ckgit::loadCiRun(fixture.state, "demo", run.run_id);
  require(loaded.has_value() && loaded->runner == "uconsole" && loaded->attempt == 2,
          "runner and attempt survive the run record");

  // A record written by the previous schema still loads, as attempt 1 of no runner.
  const std::string old_id = ckgit::generateCiId();
  const std::filesystem::path old_dir = fixture.state / "ci" / "runs" / "demo" / old_id;
  std::filesystem::create_directories(old_dir);
  std::filesystem::permissions(old_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  {
    std::ofstream out(old_dir / "run.ini");
    out << "schema_version=2\nrun_id=" << old_id << "\nproject=demo\nref_hex=" << ckgit::hexEncode("refs/heads/main")
        << "\ncommit=" << std::string(40, 'b') << "\nstatus=success\nstarted_epoch=1\nfinished_epoch=2\n"
        << "heartbeat_epoch=2\ndetail_hex=\nstep_count=0\n";
  }
  std::filesystem::permissions(old_dir / "run.ini", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  const auto old = ckgit::loadCiRun(fixture.state, "demo", old_id);
  require(old.has_value() && old->runner.empty() && old->attempt == 1, "a schema-2 record still loads");

  ckgit::CiRunReport report;
  report.run = *loaded;
  report.run.detail = "running";
  const ckgit::CiRunReport shown =
      ckgit::parseCiRunControlResponse(ckgit::formatCiRunControlResponse(report), "demo");
  require(shown.run.detail == "running (on uconsole, attempt 2)", "ckgit ci show names the runner: " + shown.run.detail);

  ckgit::ProjectSummary project;
  project.name = "demo";
  project.ci_runs.push_back(*loaded);
  const std::string page = ckgit::renderCiRunDetail(project, *loaded, std::nullopt, 0, "nonce");
  require(page.find("<dt>Runner</dt>") != std::string::npos && page.find("uconsole") != std::string::npos &&
              page.find("<dt>Attempt</dt><dd>2") != std::string::npos,
          "the run page shows the runner and the attempt");
  require(ckgit::renderCiRuns(project).find("on uconsole") != std::string::npos, "the run list shows the runner");

  ckgit::CiRunnerRecord registered;
  registered.name = "uconsole";
  registered.version = "0.6.0";
  registered.last_seen_epoch_seconds = 1000;
  ckgit::CiClaim claim;
  claim.job = sampleJob(run.run_id);
  claim.lease.runner = "uconsole";
  const std::string runners = ckgit::renderCiRunners({registered}, {claim}, 1010);
  require(runners.find("primary") != std::string::npos && runners.find("uconsole") != std::string::npos &&
              runners.find("0.6.0") != std::string::npos && runners.find("/project/demo/ci/" + run.run_id) != std::string::npos,
          "the runners page lists the server, the remote runner, and its current job");
  require(ckgit::parseHttpRoute("/ci/runners").kind == ckgit::RouteKind::kCiRunners, "the runners page has a route");
}


// A page showing the end of a step log reads only that end, however large the
// log grew (the live view of a long build).
void testLogTail() {
  Fixture fixture;
  ckgit::CiJobRequest job = sampleJob(ckgit::generateCiId());
  ckgit::enqueueCiJob(fixture.state, job);
  const std::filesystem::path log = fixture.state / "ci" / "runs" / "demo" / job.job_id / "steps" / "0.log";
  {
    std::ofstream out(log, std::ios::binary);
    out << std::string(3u << 20, 'x') << "last line\n";
  }
  require(chmod(log.c_str(), 0600) == 0, "could not secure the log");
  require(!ckgit::readCiRunLog(fixture.state, "demo", job.job_id, 0, 1u << 20).has_value(),
          "a whole-file read refuses a log over its cap");
  const auto tail = ckgit::readCiRunLogTail(fixture.state, "demo", job.job_id, 0, 64);
  require(tail.has_value() && tail->bytes.size() == 64 && tail->total == (3u << 20) + 10 &&
              tail->bytes.substr(tail->bytes.size() - 10) == "last line\n",
          "the tail of a large log is read with its full size");
  require(!ckgit::readCiRunLogTail(fixture.state, "demo", job.job_id, 1, 64).has_value(), "a missing step log has no tail");
}

}  // namespace

void testCiDistributed() {
  testDirectoryWatchSeesQueuedJob();
  testDirectoryWatchInactiveSleeps();
  testLeaseClaimRenewRelease();
  testReaperRequeuesThenGivesUp();
  testRestartAndAbandon();
  testVersionOneJobStillClaims();
  testStagedUploadResume();
  testCommitVerifiesStagedOutputs();
  testPagesOrderingGuard();
  testClaimDelay();
  testOneRunPerRef();
  testRunnerCommandGrammar();
  testProtocolRoundTrips();
  testGatewayRun();
  testGatewayFinishesSkippedJobs();
  testRunnerKeyLine();
  testRunnerConfig();
  testStreamEdges();
  testRunnerRegistry();
  testRunnerShownOnRuns();
  testLogTail();
}
