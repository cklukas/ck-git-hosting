// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_store.hpp"

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

}  // namespace

void testCiStore() {
  testIdsAndStatus();
  testSpoolFifo();
  testMalformedSpoolSkipped();
  testRunRecordRoundTrip();
  testRunsNewestFirstAndCap();
}
