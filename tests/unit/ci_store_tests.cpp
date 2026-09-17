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

void writeReleaseWithAsset(const std::filesystem::path& state, const std::string& tag,
                           const std::string& notes, const std::string& asset, std::uint64_t bytes,
                           std::uint64_t created) {
  const std::filesystem::path dir = ckgit::prepareCiReleaseDirectory(state, "demo", tag);
  const std::filesystem::path blob = dir / (asset + ".tar");
  {
    std::ofstream out(blob, std::ios::binary);
    out << std::string(static_cast<std::size_t>(bytes), 'x');
  }
  require(chmod(blob.c_str(), 0600) == 0, "could not secure the fixture release asset");
  ckgit::CiArtifactRecord record;
  record.name = asset;
  record.bytes = bytes;
  record.expires_epoch_seconds = 0;  // durable
  ckgit::writeCiReleaseArtifactRecord(state, "demo", tag, record);
  ckgit::CiReleaseRecord release;
  release.tag = tag;
  release.commit_id = std::string(40, 'a');
  release.created_epoch_seconds = created;
  release.notes = notes;
  ckgit::writeCiReleaseRecord(state, "demo", release);
}

void testReleaseRoundTrip() {
  StoreFixture fixture;
  writeReleaseWithAsset(fixture.state, "v1.0.0", "First release", "app", 20, 2000);
  writeReleaseWithAsset(fixture.state, "v0.9.0", "", "app", 10, 1000);
  const auto releases = ckgit::loadReleases(fixture.state, "demo");
  require(releases.size() == 2, "two releases load");
  require(releases[0].tag == "v1.0.0" && releases[1].tag == "v0.9.0", "releases are newest-first by creation");
  require(releases[0].notes == "First release", "release notes round trip");
  require(releases[0].assets.size() == 1 && releases[0].assets[0].name == "app" &&
              releases[0].assets[0].expires_epoch_seconds == 0,
          "the release asset is durable");
  const auto blob = ckgit::readCiReleaseAsset(fixture.state, "demo", "v1.0.0", "app", 1u << 20);
  require(blob.has_value() && blob->size() == 20, "the release asset downloads with its bytes");
  require(!ckgit::readCiReleaseAsset(fixture.state, "demo", "v1.0.0", "../escape", 1u << 20).has_value(),
          "a traversal asset name is rejected");
}

void testReleaseRemovalCascades() {
  StoreFixture fixture;
  writeReleaseWithAsset(fixture.state, "v1", "", "app", 10, 1000);
  writeReleaseWithAsset(fixture.state, "v2", "", "app", 10, 2000);
  ckgit::removeCiRelease(fixture.state, "demo", "v1");
  const auto releases = ckgit::loadReleases(fixture.state, "demo");
  require(releases.size() == 1 && releases[0].tag == "v2", "removeCiRelease drops one release");
  require(!ckgit::readCiReleaseAsset(fixture.state, "demo", "v1", "app", 1u << 20).has_value(),
          "the removed release's asset is gone");
  ckgit::removeProjectCi(fixture.state, "demo");
  require(ckgit::loadReleases(fixture.state, "demo").empty(), "removeProjectCi cascades to releases");
}

void testReleaseTagValidation() {
  require(ckgit::isValidReleaseTag("v1.0.0"), "a version tag is valid");
  require(ckgit::isValidReleaseTag("release-1_2"), "letters, digits, -._ are valid");
  require(!ckgit::isValidReleaseTag(""), "empty tag rejected");
  require(!ckgit::isValidReleaseTag(".."), "'..' rejected");
  require(!ckgit::isValidReleaseTag("a/b"), "slashed tag rejected");
}

// D1/WP1: "release" is reserved for release.ini itself; writing an asset
// record under that name must fail rather than silently overwrite it. The
// workflow parser already refuses the name (ci_workflow_tests.cpp), so this
// is defence in depth for a direct store caller.
void testReservedReleaseArtifactName() {
  StoreFixture fixture;
  const std::filesystem::path dir = ckgit::prepareCiReleaseDirectory(fixture.state, "demo", "v1");
  {
    std::ofstream out(dir / "release.tar", std::ios::binary);
    out << "x";
  }
  require(chmod((dir / "release.tar").c_str(), 0600) == 0, "could not secure the fixture asset");
  ckgit::CiArtifactRecord record;
  record.name = "release";
  record.bytes = 1;
  record.expires_epoch_seconds = 0;
  bool threw = false;
  try {
    ckgit::writeCiReleaseArtifactRecord(fixture.state, "demo", "v1", record);
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "writing a release asset named 'release' is refused");
}

// WP7: the `releases` control response line format
// (docs/protocol/01-ssh-and-control-v1.md) built from real loadReleases
// output round-trips through the client-side parser, and the parser rejects
// the malformed shapes a buggy or hostile server could send.
void testReleasesControlResponseRoundTrip() {
  StoreFixture fixture;
  const std::filesystem::path dir = ckgit::prepareCiReleaseDirectory(fixture.state, "demo", "v1.0.0");
  {
    std::ofstream out(dir / "packages.tar", std::ios::binary);
    out << std::string(20, 'x');
  }
  require(chmod((dir / "packages.tar").c_str(), 0600) == 0, "could not secure the fixture release asset");
  ckgit::CiArtifactRecord asset;
  asset.name = "packages";
  asset.bytes = 20;
  asset.sha256 = std::string(64, 'a');
  asset.expires_epoch_seconds = 0;
  ckgit::writeCiReleaseArtifactRecord(fixture.state, "demo", "v1.0.0", asset);
  ckgit::CiReleaseRecord release;
  release.tag = "v1.0.0";
  release.commit_id = std::string(40, 'b');
  release.created_epoch_seconds = 2000;
  ckgit::writeCiReleaseRecord(fixture.state, "demo", release);
  writeReleaseWithAsset(fixture.state, "v0.9.0", "", "app", 10, 1000);  // asset with no sha256 on record

  const auto releases = ckgit::loadReleases(fixture.state, "demo");
  require(releases.size() == 2, "two fixture releases load");
  std::string wire = "ok " + std::to_string(releases.size()) + "\n";
  for (const auto& entry : releases) {
    wire += "release " + entry.tag + " " + entry.commit_id + " " + std::to_string(entry.created_epoch_seconds) + "\n";
    for (const auto& entry_asset : entry.assets) {
      wire += "asset " + entry.tag + " " + entry_asset.name + " " + std::to_string(entry_asset.bytes) + " " +
              (entry_asset.sha256.empty() ? "-" : entry_asset.sha256) + "\n";
    }
  }
  const auto parsed = ckgit::parseReleasesControlResponse(wire);
  require(parsed.size() == 2, "both releases round trip through the wire format");
  require(parsed[0].tag == "v1.0.0" && parsed[0].commit_id == releases[0].commit_id &&
              parsed[0].created_epoch_seconds == 2000,
          "release fields round trip");
  require(parsed[0].assets.size() == 1 && parsed[0].assets[0].name == "packages" &&
              parsed[0].assets[0].bytes == 20 && parsed[0].assets[0].sha256 == std::string(64, 'a'),
          "asset fields including sha256 round trip");
  require(parsed[1].tag == "v0.9.0" && parsed[1].assets.size() == 1 && parsed[1].assets[0].sha256.empty(),
          "a '-' sha256 field parses back to empty, not the literal dash");

  const auto rejects = [](std::string_view response) {
    try {
      static_cast<void>(ckgit::parseReleasesControlResponse(response));
    } catch (const std::exception&) {
      return true;
    }
    return false;
  };
  require(rejects("ok 2\nrelease v1 " + std::string(40, 'a') + " 1\n"), "count mismatch is rejected");
  require(rejects("ok 1\nasset v1 app 1 -\n"), "an asset before any release is rejected");
  require(rejects("ok 1\nrelease v1 " + std::string(40, 'a') + " 1\nasset other app 1 -\n"),
          "an asset whose tag does not match its release is rejected");
  require(rejects("ok 1\nrelease v1 not-hex 1\n"), "a non-hex commit id is rejected");
  require(rejects("ok 1\nrelease v1 " + std::string(40, 'a') + " 1\nasset v1 app 1 " + std::string(63, 'a') + "\n"),
          "a short sha256 is rejected");
  require(rejects("ok 1\nbogus v1\n"), "an unrecognized record prefix is rejected");
  require(ckgit::parseReleasesControlResponse("ok 0\n").empty(), "zero releases parses to an empty list");
}

void testStatusHelpers() {
  require(ckgit::ciRunStatusName(ckgit::CiRunStatus::Cancelled) == "cancelled", "cancelled has a name");
  const auto parsed = ckgit::ciRunStatusFromName("cancelled");
  require(parsed.has_value() && *parsed == ckgit::CiRunStatus::Cancelled, "cancelled parses back");
  require(ckgit::ciRunStatusIsActive(ckgit::CiRunStatus::Running) &&
              ckgit::ciRunStatusIsActive(ckgit::CiRunStatus::Pending),
          "running and pending are active");
  require(!ckgit::ciRunStatusIsActive(ckgit::CiRunStatus::Cancelled) &&
              !ckgit::ciRunStatusIsActive(ckgit::CiRunStatus::Success),
          "terminal statuses are not active");
  require(!ckgit::ciRunStatusIcon(ckgit::CiRunStatus::Cancelled).empty() &&
              !ckgit::ciRunStatusIcon(ckgit::CiRunStatus::Running).empty(),
          "every status has an icon glyph");
}

void testLiveRecordHeartbeatAndSingleLoad() {
  StoreFixture fixture;
  ckgit::CiRunRecord record;
  record.run_id = "00000000000000000200-aabbccdd";
  record.project_name = "demo";
  record.ref = "refs/heads/main";
  record.commit_id = std::string(40, 'a');
  record.status = ckgit::CiRunStatus::Running;
  record.started_epoch_seconds = 1700000000;
  record.heartbeat_epoch_seconds = 1700000050;
  record.steps.push_back({"build", 0, false, false});

  ckgit::prepareCiRunDirectory(fixture.state, "demo", record.run_id);
  ckgit::writeCiRunRecord(fixture.state, record);

  const auto single = ckgit::loadCiRun(fixture.state, "demo", record.run_id);
  require(single.has_value(), "loadCiRun reads one run fresh");
  require(single->status == ckgit::CiRunStatus::Running, "running status round trips");
  require(single->heartbeat_epoch_seconds == 1700000050, "heartbeat round trips (schema v2)");
  require(single->steps.size() == 1, "in-progress steps round trip");
  require(!ckgit::loadCiRun(fixture.state, "demo", "00000000000000009999-deadbeef").has_value(),
          "loadCiRun is empty for an unknown run");
}

void testSchemaVersionOneBackCompat() {
  StoreFixture fixture;
  const std::string run_id = "00000000000000000300-1a2b3c4d";
  const auto dir = ckgit::prepareCiRunDirectory(fixture.state, "demo", run_id);
  const auto hex = [](const std::string& value) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : value) {
      out += digits[byte >> 4];
      out += digits[byte & 0x0f];
    }
    return out;
  };
  // A record with no heartbeat line, exactly as an earlier build wrote it.
  const std::string content = "schema_version=1\nrun_id=" + run_id +
      "\nproject=demo\nref_hex=" + hex("refs/heads/main") + "\ncommit=" + std::string(40, 'a') +
      "\nstatus=success\nstarted_epoch=1700000000\nfinished_epoch=1700000100\ndetail_hex=" +
      hex("all steps passed") + "\nstep_count=1\nstep=0,0,0," + hex("build") + "\n";
  {
    std::ofstream file(dir / "run.ini");
    file << content;
  }
  require(chmod((dir / "run.ini").c_str(), 0600) == 0, "fixture record made private");
  const auto loaded = ckgit::loadCiRun(fixture.state, "demo", run_id);
  require(loaded.has_value(), "a version-1 record still parses after upgrade");
  require(loaded->status == ckgit::CiRunStatus::Success, "version-1 status preserved");
  require(loaded->heartbeat_epoch_seconds == 0, "a version-1 record has no heartbeat");
  require(loaded->detail == "all steps passed" && loaded->steps.size() == 1, "version-1 body preserved");
}

void testCancelMarkerLifecycle() {
  StoreFixture fixture;
  const std::string run_id = "00000000000000000400-55667788";
  ckgit::prepareCiRunDirectory(fixture.state, "demo", run_id);
  require(!ckgit::isCiCancelRequested(fixture.state, "demo", run_id), "no cancel marker initially");
  require(ckgit::requestCiCancel(fixture.state, "demo", run_id), "cancel accepted for an existing run");
  require(ckgit::isCiCancelRequested(fixture.state, "demo", run_id), "the cancel marker is observed");
  ckgit::clearCiCancel(fixture.state, "demo", run_id);
  require(!ckgit::isCiCancelRequested(fixture.state, "demo", run_id), "clearCiCancel removes the marker");
  require(!ckgit::requestCiCancel(fixture.state, "demo", "00000000000000009999-deadbeef"),
          "cancel is refused for an unknown run");
}

void testLogChunkTailing() {
  StoreFixture fixture;
  const std::string run_id = "00000000000000000500-1234abcd";
  const auto dir = ckgit::prepareCiRunDirectory(fixture.state, "demo", run_id);
  {
    std::ofstream log(dir / "steps" / "0.log");
    log << "line one\nline two\n";  // 18 bytes; "line one\n" is the first 9
  }
  require(chmod((dir / "steps" / "0.log").c_str(), 0600) == 0, "step log made private");
  const auto whole = ckgit::readCiRunLogChunk(fixture.state, "demo", run_id, 0, 0, 1u << 20);
  require(whole.has_value() && *whole == "line one\nline two\n", "reads the whole log from offset 0");
  const auto tail = ckgit::readCiRunLogChunk(fixture.state, "demo", run_id, 0, 9, 1u << 20);
  require(tail.has_value() && *tail == "line two\n", "reads only the bytes appended past the offset");
  const auto none = ckgit::readCiRunLogChunk(fixture.state, "demo", run_id, 0, 18, 1u << 20);
  require(none.has_value() && none->empty(), "no bytes past the end returns empty, not absent");
  const auto capped = ckgit::readCiRunLogChunk(fixture.state, "demo", run_id, 0, 0, 4);
  require(capped.has_value() && *capped == "line", "honours the byte cap");
  require(!ckgit::readCiRunLogChunk(fixture.state, "demo", run_id, 1, 0, 1u << 20).has_value(),
          "an absent step log reads as std::nullopt");
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
  testReleaseRoundTrip();
  testReleaseRemovalCascades();
  testReleaseTagValidation();
  testReservedReleaseArtifactName();
  testReleasesControlResponseRoundTrip();
  testStatusHelpers();
  testLiveRecordHeartbeatAndSingleLoad();
  testSchemaVersionOneBackCompat();
  testCancelMarkerLifecycle();
  testLogChunkTailing();
}
