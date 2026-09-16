// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_store.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci store: " + message);
}

class StoreFixture {
 public:
  StoreFixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    auto pattern = (std::filesystem::path(configured_tmp) / "ckgit-ci-store-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create isolated fixture");
    root = writable.data();
    state = root / "state";
    std::filesystem::create_directories(state);
    require(chmod(state.c_str(), 0700) == 0, "could not secure fixture state");
  }
  ~StoreFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path root;
  std::filesystem::path state;
};

ckgit::CiJobRequest sampleJob(const std::string& id) {
  ckgit::CiJobRequest job;
  job.job_id = id;
  job.project_name = "demo";
  job.ref = "refs/heads/main";
  job.commit_id = std::string(40, 'a');
  job.client_id = "mac-studio";
  job.queued_epoch_seconds = 1700000000;
  return job;
}

void testIdsAndStatus() {
  require(ckgit::isValidCiId("00000000000000000001-aabbccdd"), "a generated-style id is valid");
  require(!ckgit::isValidCiId(""), "empty id rejected");
  require(!ckgit::isValidCiId("../escape"), "traversal id rejected");
  require(!ckgit::isValidCiId("has space"), "space id rejected");
  const std::string generated = ckgit::generateCiId();
  require(ckgit::isValidCiId(generated), "generateCiId produces a valid id");
  require(ckgit::ciRunStatusFromName("success") == ckgit::CiRunStatus::Success, "status round trip");
  require(!ckgit::ciRunStatusFromName("bogus").has_value(), "unknown status rejected");
}

void testSpoolFifo() {
  StoreFixture fixture;
  ckgit::enqueueCiJob(fixture.state, sampleJob("00000000000000000001-aaaaaaaa"));
  ckgit::enqueueCiJob(fixture.state, sampleJob("00000000000000000002-bbbbbbbb"));

  const auto first = ckgit::claimNextCiJob(fixture.state);
  require(first.has_value() && first->job_id == "00000000000000000001-aaaaaaaa", "oldest job claimed first");
  require(first->ref == "refs/heads/main" && first->commit_id == std::string(40, 'a'), "job fields round trip");
  const auto second = ckgit::claimNextCiJob(fixture.state);
  require(second.has_value() && second->job_id == "00000000000000000002-bbbbbbbb", "second job claimed next");
  require(!ckgit::claimNextCiJob(fixture.state).has_value(), "empty spool yields nothing");

  ckgit::releaseCiJob(fixture.state, "00000000000000000001-aaaaaaaa");
  ckgit::releaseCiJob(fixture.state, "00000000000000000002-bbbbbbbb");  // idempotent-ish
}

void testMalformedSpoolSkipped() {
  StoreFixture fixture;
  const std::filesystem::path spool = fixture.state / "ci" / "spool";
  std::filesystem::create_directories(spool);
  require(chmod((fixture.state / "ci").c_str(), 0700) == 0, "secure ci dir");
  require(chmod(spool.c_str(), 0700) == 0, "secure spool dir");
  {
    std::ofstream bad(spool / "00000000000000000000-deadbeef.ini");
    bad << "this is not a valid job record\n";
  }
  require(!ckgit::claimNextCiJob(fixture.state).has_value(), "a malformed spool entry is skipped");
  // A good job queued afterwards is still claimable.
  ckgit::enqueueCiJob(fixture.state, sampleJob("00000000000000000009-cccccccc"));
  const auto claimed = ckgit::claimNextCiJob(fixture.state);
  require(claimed.has_value() && claimed->job_id == "00000000000000000009-cccccccc", "good job claimed after a bad one");
}

void testRunRecordRoundTrip() {
  StoreFixture fixture;
  ckgit::CiRunRecord record;
  record.run_id = "00000000000000000100-11223344";
  record.project_name = "demo";
  record.ref = "refs/heads/main";
  record.commit_id = std::string(64, 'f');
  record.status = ckgit::CiRunStatus::Failure;
  record.started_epoch_seconds = 1700000000;
  record.finished_epoch_seconds = 1700000042;
  record.detail = "step 'tests' failed with exit 2";
  record.steps.push_back({"build", 0, false, false});
  record.steps.push_back({"tests", 2, false, true});

  ckgit::prepareCiRunDirectory(fixture.state, "demo", record.run_id);
  ckgit::writeCiRunRecord(fixture.state, record);

  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1, "one run loaded");
  const ckgit::CiRunRecord& loaded = runs[0];
  require(loaded.run_id == record.run_id && loaded.commit_id == record.commit_id, "identity round trip");
  require(loaded.status == ckgit::CiRunStatus::Failure, "status round trip");
  require(loaded.ref == "refs/heads/main", "ref round trip");
  require(loaded.detail == record.detail, "detail round trip");
  require(loaded.steps.size() == 2 && loaded.steps[1].name == "tests" && loaded.steps[1].exit_code == 2 &&
              loaded.steps[1].output_truncated,
          "steps round trip");
}

void testRunsNewestFirstAndCap() {
  StoreFixture fixture;
  for (int index = 1; index <= 5; ++index) {
    ckgit::CiRunRecord record;
    record.run_id = "0000000000000000000" + std::to_string(index) + "-abcdef01";
    record.project_name = "demo";
    record.ref = "refs/heads/main";
    record.commit_id = std::string(40, 'a');
    record.status = ckgit::CiRunStatus::Success;
    ckgit::prepareCiRunDirectory(fixture.state, "demo", record.run_id);
    ckgit::writeCiRunRecord(fixture.state, record);
  }
  const auto newest = ckgit::loadCiRuns(fixture.state, "demo", 3);
  require(newest.size() == 3, "maximum respected");
  require(newest[0].run_id == "00000000000000000005-abcdef01", "newest first");
  require(newest[2].run_id == "00000000000000000003-abcdef01", "descending order");
  require(ckgit::loadCiRuns(fixture.state, "demo", 0).empty(), "zero maximum yields nothing");
  require(ckgit::loadCiRuns(fixture.state, "absent").empty(), "unknown project yields nothing");
}

void testProjectOptIn() {
  StoreFixture fixture;
  require(!ckgit::isProjectCiEnabled(fixture.state, "demo"), "CI is off until opted in");
  ckgit::setProjectCiEnabled(fixture.state, "demo", true);
  require(ckgit::isProjectCiEnabled(fixture.state, "demo"), "enabling opts the project in");
  require(!ckgit::isProjectCiEnabled(fixture.state, "other"), "opt-in is per project");
  ckgit::setProjectCiEnabled(fixture.state, "demo", false);
  require(!ckgit::isProjectCiEnabled(fixture.state, "demo"), "disabling opts back out");

  ckgit::setProjectCiEnabled(fixture.state, "demo", true);
  ckgit::enqueueCiJob(fixture.state, sampleJob("00000000000000000042-abcdabcd"));
  ckgit::CiRunRecord record;
  record.run_id = "00000000000000000043-abcdabcd";
  record.project_name = "demo";
  record.commit_id = std::string(40, 'a');
  record.status = ckgit::CiRunStatus::Success;
  ckgit::prepareCiRunDirectory(fixture.state, "demo", record.run_id);
  ckgit::writeCiRunRecord(fixture.state, record);
  ckgit::removeProjectCi(fixture.state, "demo");
  require(!ckgit::isProjectCiEnabled(fixture.state, "demo"), "removeProjectCi clears the opt-in");
  require(ckgit::loadCiRuns(fixture.state, "demo").empty(), "removeProjectCi clears the run history");
}

// Records a successful run and one artifact bundle of `bytes` bytes. The blob is
// forced to 0600 so the private-dir read accepts it, matching what the runner
// writes via open(…, 0600).
void writeRunWithArtifact(const std::filesystem::path& state, const std::string& run_id,
                          const std::string& name, std::uint64_t bytes, std::uint64_t created,
                          std::uint64_t expires) {
  ckgit::CiRunRecord record;
  record.run_id = run_id;
  record.project_name = "demo";
  record.commit_id = std::string(40, 'a');
  record.status = ckgit::CiRunStatus::Success;
  ckgit::prepareCiRunDirectory(state, "demo", run_id);
  ckgit::writeCiRunRecord(state, record);
  const std::filesystem::path dir = ckgit::prepareCiArtifactDirectory(state, "demo", run_id);
  const std::filesystem::path blob = dir / (name + ".tar");
  {
    std::ofstream out(blob, std::ios::binary);
    out << std::string(static_cast<std::size_t>(bytes), 'x');
  }
  require(chmod(blob.c_str(), 0600) == 0, "could not secure the fixture artifact");
  ckgit::CiArtifactRecord artifact;
  artifact.name = name;
  artifact.bytes = bytes;
  artifact.created_epoch_seconds = created;
  artifact.expires_epoch_seconds = expires;
  ckgit::writeCiArtifactRecord(state, "demo", run_id, artifact);
}

void testArtifactRoundTrip() {
  StoreFixture fixture;
  writeRunWithArtifact(fixture.state, "00000000000000000001-aaaaaaaa", "bundle", 32, 1000, 2000);
  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1 && runs[0].artifacts.size() == 1, "the artifact surfaces in the run");
  require(runs[0].artifacts[0].name == "bundle" && runs[0].artifacts[0].bytes == 32 &&
              runs[0].artifacts[0].expires_epoch_seconds == 2000,
          "artifact fields round trip");
  const auto blob = ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "bundle", 1u << 20);
  require(blob.has_value() && blob->size() == 32, "the artifact downloads with its stored bytes");
  require(!ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "missing", 1u << 20).has_value(),
          "an absent artifact yields nothing");
  require(!ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "../escape", 1u << 20).has_value(),
          "a traversal artifact name is rejected");
}

void testArtifactSweep() {
  const std::uint64_t future = 9999999999ull;
  // Timer expiry, with keep-latest protecting the newest run.
  {
    StoreFixture fixture;
    writeRunWithArtifact(fixture.state, "00000000000000000001-aaaaaaaa", "old", 10, 100, 1000);
    writeRunWithArtifact(fixture.state, "00000000000000000002-aaaaaaaa", "new", 10, 200, future);
    ckgit::CiArtifactSweepOptions options;
    options.now_epoch_seconds = 5000;
    require(ckgit::sweepCiArtifacts(fixture.state, options) == 1, "one expired artifact is removed");
    require(!ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "old", 1u << 20).has_value(),
            "the expired artifact is gone");
    require(ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000002-aaaaaaaa", "new", 1u << 20).has_value(),
            "the newest artifact is kept");
  }
  // keep-latest protects an expired newest run; without it, the artifact goes.
  {
    StoreFixture fixture;
    writeRunWithArtifact(fixture.state, "00000000000000000001-aaaaaaaa", "only", 10, 100, 1000);
    ckgit::CiArtifactSweepOptions options;
    options.now_epoch_seconds = 5000;
    require(ckgit::sweepCiArtifacts(fixture.state, options) == 0, "keep-latest protects the newest run");
    require(ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "only", 1u << 20).has_value(),
            "the newest run's artifact survives despite expiry");
    options.keep_latest = false;
    require(ckgit::sweepCiArtifacts(fixture.state, options) == 1, "without keep-latest it is removed");
  }
  // Budget eviction, oldest first, until under the total budget.
  {
    StoreFixture fixture;
    writeRunWithArtifact(fixture.state, "00000000000000000001-aaaaaaaa", "a", 1000, 100, future);
    writeRunWithArtifact(fixture.state, "00000000000000000002-aaaaaaaa", "b", 1000, 200, future);
    writeRunWithArtifact(fixture.state, "00000000000000000003-aaaaaaaa", "c", 1000, 300, future);
    ckgit::CiArtifactSweepOptions options;
    options.now_epoch_seconds = 5000;
    options.keep_latest = false;
    options.max_total_bytes = 1500;
    require(ckgit::sweepCiArtifacts(fixture.state, options) == 2, "eviction drops the two oldest");
    require(!ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000001-aaaaaaaa", "a", 1u << 20).has_value(),
            "the oldest is evicted");
    require(ckgit::readCiArtifact(fixture.state, "demo", "00000000000000000003-aaaaaaaa", "c", 1u << 20).has_value(),
            "the newest survives eviction");
  }
  // runs_keep prunes old run directories entirely.
  {
    StoreFixture fixture;
    for (int index = 1; index <= 5; ++index) {
      writeRunWithArtifact(fixture.state, "0000000000000000000" + std::to_string(index) + "-aaaaaaaa", "x", 10,
                           static_cast<std::uint64_t>(100 * index), future);
    }
    ckgit::CiArtifactSweepOptions options;
    options.now_epoch_seconds = 5000;
    options.runs_keep = 2;
    ckgit::sweepCiArtifacts(fixture.state, options);
    const auto runs = ckgit::loadCiRuns(fixture.state, "demo", 100);
    require(runs.size() == 2, "runs_keep prunes to the newest run directories");
    require(runs[0].run_id == "00000000000000000005-aaaaaaaa", "the newest run is kept");
  }
}

}  // namespace

void testCiStore() {
  testIdsAndStatus();
  testSpoolFifo();
  testMalformedSpoolSkipped();
  testRunRecordRoundTrip();
  testRunsNewestFirstAndCap();
  testProjectOptIn();
  testArtifactRoundTrip();
  testArtifactSweep();
}
