// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Unit tests for the pieces that let several CI runners share one server's
// queue: spool notifications, leases and requeueing, claim scheduling, and the
// runner protocol.

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

#include "ckgit/ci_results.hpp"
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
}
