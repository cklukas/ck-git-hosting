// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_runner.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "ckgit/ci_store.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/process.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci runner: " + message);
}

class RunnerFixture {
 public:
  RunnerFixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    auto pattern = (std::filesystem::path(configured_tmp) / "ckgit-ci-runner-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create isolated fixture");
    root = writable.data();
    repo = root / "repo";
    state = root / "state";
    build = root / "build";
    std::filesystem::create_directories(repo);
    std::filesystem::create_directories(state);
    require(chmod(state.c_str(), 0700) == 0, "could not secure fixture state");
    git({"-c", "init.defaultBranch=main", "init", "-q"});
  }
  ~RunnerFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  void git(std::initializer_list<std::string> arguments) {
    std::vector<std::string> command{"git", "-C", repo.string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto result = ckgit::runProcess(command, std::chrono::seconds(20));
    if (result.exit_code != 0 || result.timed_out) {
      throw std::runtime_error("test git failed: " + result.output);
    }
  }

  // Commits `content` at .ckgit/ci.yml (or `path` when given) and returns the commit id.
  std::string commit(const std::string& content, const std::string& path = ".ckgit/ci.yml") {
    const std::filesystem::path file = repo / path;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << content;
    git({"add", "-A"});
    git({"-c", "user.email=t@example.invalid", "-c", "user.name=Test", "commit", "-q", "-m", "workflow"});
    const auto rev = ckgit::runProcess({"git", "-C", repo.string(), "rev-parse", "HEAD"},
                                       std::chrono::seconds(10));
    require(rev.exit_code == 0, "rev-parse failed");
    std::string id = rev.output;
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
    return id;
  }

  ckgit::CiRunnerOptions options(const std::string& commit_id) {
    ckgit::CiRunnerOptions opts;
    opts.repository = repo / ".git";
    opts.commit_id = commit_id;
    opts.project_name = "demo";
    opts.ref = "refs/heads/main";
    opts.state_root = state;
    opts.build_root = build;
    opts.timeout_seconds = 30;
    return opts;
  }

  std::filesystem::path stepLog(const ckgit::CiRunRecord& record, int index) const {
    return state / "ci" / "runs" / "demo" / record.run_id / "steps" / (std::to_string(index) + ".log");
  }

  std::filesystem::path root, repo, state, build;
};

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The absolute path of this very test binary, so a workflow step can
// re-invoke it (with --loopback-probe; see test_main.cpp) without depending
// on an external tool like `nc` being on the runner's scrubbed PATH.
std::string thisExecutablePath() {
#if defined(__linux__)
  return std::filesystem::canonical("/proc/self/exe").string();
#elif defined(__APPLE__)
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) throw std::runtime_error("executable path buffer too small");
  return std::filesystem::canonical(buffer).string();
#else
  throw std::runtime_error("thisExecutablePath is not implemented on this platform");
#endif
}

// Prefixes every line of `text` (assumed to already end in a newline) with
// `indent` spaces, so a multi-line shell script can be embedded as a `|`
// block-literal scalar in a workflow string built in C++, without juggling
// nested YAML/shell quoting for a script that itself contains quotes or
// interpolates absolute paths.
std::string indentBlock(const std::string& text, std::size_t indent) {
  const std::string prefix(indent, ' ');
  std::string out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end = newline == std::string::npos ? text.size() : newline;
    out += prefix;
    out.append(text, start, end - start);
    out += '\n';
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  return out;
}

void testSuccess() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - name: greet\n"
      "        run: echo hello-from-ci\n"
      "      - run: [true]\n");
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id));
  require(record.status == ckgit::CiRunStatus::Success,
          "a passing workflow succeeds (detail=" + record.detail + ")");
  require(record.steps.size() == 2, "both steps recorded");
  require(record.steps[0].name == "greet" && record.steps[0].exit_code == 0, "first step ok");
  require(readFile(fixture.stepLog(record, 0)).find("hello-from-ci") != std::string::npos,
          "the step's output is captured to its log");
  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1 && runs[0].status == ckgit::CiRunStatus::Success, "the run is persisted");
}

void testFailureStops() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: sh -ec 'exit 3'\n"
      "      - run: echo should-not-run\n");
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id));
  require(record.status == ckgit::CiRunStatus::Failure, "a non-zero step fails the run");
  require(record.steps.size() == 1, "the run stops at the first failing step");
  require(record.steps[0].exit_code == 3, "the exit code is recorded");
}

void testTimeout() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: slow\n"
      "    steps:\n"
      "      - run: [sleep, '30']\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.timeout_seconds = 1;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Timeout, "a step over budget times out");
  require(record.steps.size() == 1 && record.steps[0].timed_out, "the timeout is recorded on the step");
}

void testOutputCap() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: noisy\n"
      "    steps:\n"
      "      - run: sh -ec \"head -c 200000 /dev/zero | tr '\\\\0' x\"\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.max_log_bytes = 4096;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "a noisy step still succeeds");
  require(record.steps.size() == 1 && record.steps[0].output_truncated, "the log is marked truncated");
  const auto size = std::filesystem::file_size(fixture.stepLog(record, 0));
  require(size <= opts.max_log_bytes, "the captured log does not exceed the cap");
}

// D2/WP2: even with the LAN denied by default, a step must still reach
// 127.0.0.1/::1 (its own local server, database, or test fixture). Before the
// fix a freshly unshared network namespace left loopback down, so this step
// would fail with status Failure on any host where namespaces are actually
// enforced (Linux with unprivileged user namespaces enabled); in degraded
// mode (macOS, or namespaces unavailable) the step runs unsandboxed and the
// assertion holds trivially. Either way, a real regression shows up as
// CiRunStatus::Failure here, not just as a warning on stderr.
void testLoopbackInsideSandbox() {
  // Skip when this test binary is itself already executing as a CI step:
  // CKGIT_CI is set only by our own runner's buildEnv, so this precisely
  // detects "running nested inside ck-ci-runnerd", not GitHub Actions (which
  // sets CI but not CKGIT_CI) or an ordinary local `make check`. A nested
  // step's own enterSandbox rebinds /mnt to its own scratch, so this
  // process's self-referencing path (resolved under the outer step's /mnt)
  // would no longer exist inside the nested step -- a property of self-
  // reference under a floating bind mount, not of the loopback fix itself.
  // The fix is still exercised for real: the outer step needs working
  // loopback for the other integration scripts (control_socket.sh,
  // dashboard.sh, ...), which run in the outer step's own already-unshared
  // namespace rather than spawning a further nested one.
  if (std::getenv("CKGIT_CI") != nullptr) return;
  RunnerFixture fixture;
  const std::string self = thisExecutablePath();
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: [\"" +
      self + "\", \"--loopback-probe\"]\n");
  ckgit::CiSandboxReport sandbox;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id), &sandbox);
  // Some hosted kernels permit the namespace but deny bringing lo up inside
  // it. That platform constraint is reported by the runner; the isolated
  // loopback behavior is exercised only where the capability exists.
  if (sandbox.network_isolated && !sandbox.loopback_available) return;
  require(record.status == ckgit::CiRunStatus::Success,
          "a step reaches loopback with the LAN denied (namespaces_available=" +
              std::string(sandbox.namespaces_available ? "true" : "false") +
              ", loopback_available=" + std::string(sandbox.loopback_available ? "true" : "false") + ")");
}

// D6/WP3: a step must not be able to see or write the service tree directly
// by absolute path — the private state root, the CI build root (other runs'
// scratch), and another project's cache — only its own checkout and its own
// declared cache. Each marker file below is planted outside anything this
// workflow references; if masking works, every `test ! -e` holds and the
// step (and so the run) succeeds, and if D6 regresses, one of them is
// visible again and the run fails. Meaningless without a real mount
// namespace (degraded mode never attempts any masking at all, so the
// markers would trivially still be visible there), so it is skipped rather
// than asserted in that case.
void testServiceTreeMasked() {
  RunnerFixture fixture;
  // Directly under state_root, not under state_root/ci/...: that subtree is
  // the runner's own (spool, runs, projects), which requires every directory
  // in it to be private (0700) -- a marker planted there under an ordinary
  // mkdir would collide with that check rather than testing masking.
  const std::filesystem::path state_marker = fixture.state / "marker";
  { std::ofstream(state_marker) << "secret"; }
  const std::filesystem::path sibling_run = fixture.build / "another-run";
  std::filesystem::create_directories(sibling_run);
  const std::filesystem::path build_marker = sibling_run / "marker";
  { std::ofstream(build_marker) << "secret"; }
  const std::filesystem::path cache_root = fixture.root / "cache";
  const std::filesystem::path other_cache_marker = cache_root / "other-project" / "ccache" / "marker";
  std::filesystem::create_directories(other_cache_marker.parent_path());
  { std::ofstream(other_cache_marker) << "secret"; }

  const std::string script = "test ! -e \"" + state_marker.string() +
                             "\"\n"
                             "test ! -e \"" +
                             build_marker.string() +
                             "\"\n"
                             "test ! -e \"" +
                             other_cache_marker.string() +
                             "\"\n"
                             "test -f .ckgit/ci.yml\n"
                             "echo mask-ok\n";
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - script: |\n" +
      indentBlock(script, 10));
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.cache_root = cache_root;  // exercises the hide list's cache_root entry too
  ckgit::CiSandboxReport sandbox;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts, &sandbox);
  if (!sandbox.namespaces_available) return;  // degraded mode: no masking is attempted at all
  require(record.status == ckgit::CiRunStatus::Success,
          "the service tree is masked inside a step (filesystem_masked=" +
              std::string(sandbox.filesystem_masked ? "true" : "false") + ", detail=" + record.detail + ")");
}

// D6/WP3: a persistent cache must still land at, and be readable back from,
// its real cache_root/<project>/<name> path on the host — proving the bind
// enterSandbox establishes actually carries writes through, not just that
// the fixed CKGIT_CACHE_CCACHE path resolves to *something*. A second,
// separate run then reads the same content back through the same fixed
// path, proving persistence across runs, not merely within one run's scratch.
void testCacheStillPersists() {
  RunnerFixture fixture;
  const std::filesystem::path cache_root = fixture.root / "cache";
  const std::string write_id = fixture.commit(
      "version: 1\n"
      "cache:\n"
      "  - ccache\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - script: |\n" +
      indentBlock("printf ok > \"$CKGIT_CACHE_CCACHE/marker\"\n", 10));
  ckgit::CiRunnerOptions write_opts = fixture.options(write_id);
  write_opts.cache_root = cache_root;
  const ckgit::CiRunRecord first = ckgit::runCiWorkflow(write_opts);
  require(first.status == ckgit::CiRunStatus::Success, "a step can write to its persistent cache");
  const std::filesystem::path persisted = cache_root / "demo" / "ccache" / "marker";
  require(readFile(persisted) == "ok", "the write landed at the real, persistent cache_root path");

  const std::string read_id = fixture.commit(
      "version: 1\n"
      "cache:\n"
      "  - ccache\n"
      "jobs:\n"
      "  - name: check\n"
      "    steps:\n"
      "      - script: |\n" +
      indentBlock("test \"$(cat \"$CKGIT_CACHE_CCACHE/marker\")\" = ok\n", 10));
  ckgit::CiRunnerOptions read_opts = fixture.options(read_id);
  read_opts.cache_root = cache_root;
  const ckgit::CiRunRecord second = ckgit::runCiWorkflow(read_opts);
  require(second.status == ckgit::CiRunStatus::Success,
          "a separate run reads back the same cache content through the same fixed path");
}

void testSkippedWithoutWorkflow() {
  RunnerFixture fixture;
  const std::string id = fixture.commit("just a readme\n", "README.md");
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id));
  require(record.status == ckgit::CiRunStatus::Skipped, "a commit with no workflow is skipped");
}

void testRunIdReusedFromOptions() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: [true]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.run_id = "00000000000000000777-1a2b3c4d";
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "the run still succeeds");
  require(record.run_id == "00000000000000000777-1a2b3c4d",
          "a caller-supplied run_id is reused rather than a fresh one minted");
}

void testCancelledBeforeStart() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: echo should-not-run\n");
  const std::string run_id = "00000000000000000888-deadbeef";
  // Mirrors enqueueCiJob: the dashboard's Pending record (and thus the run
  // directory a cancel marker needs) exists before the runner ever claims the
  // job.
  ckgit::prepareCiRunDirectory(fixture.state, "demo", run_id);
  require(ckgit::requestCiCancel(fixture.state, "demo", run_id), "cancel accepted while still queued");

  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.run_id = run_id;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Cancelled, "a pre-cancelled job records Cancelled");
  require(record.run_id == run_id, "the cancelled record is the same run_id, not a fresh one");
  require(record.steps.empty(), "no step ever ran: the workflow was never parsed or checked out");
  require(!ckgit::isCiCancelRequested(fixture.state, "demo", run_id),
          "the marker is cleared once the run reaches a terminal state");
}

void testBranchTrigger() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "on: { branches: [release] }\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: [true]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.ref = "refs/heads/main";  // not a trigger branch
  require(ckgit::runCiWorkflow(opts).status == ckgit::CiRunStatus::Skipped,
          "a non-trigger branch is skipped");
  opts.ref = "refs/heads/release";
  require(ckgit::runCiWorkflow(opts).status == ckgit::CiRunStatus::Success, "a trigger branch runs");
}

void testDefaultBranchTrigger() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: [true]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.ref = "refs/heads/feature";  // not the repository's default branch (main)
  require(ckgit::runCiWorkflow(opts).status == ckgit::CiRunStatus::Skipped,
          "without on:, a non-default branch is skipped");
  opts.ref = "refs/heads/main";
  require(ckgit::runCiWorkflow(opts).status == ckgit::CiRunStatus::Success,
          "without on:, the default branch runs");
}

void testArtifacts() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p out && printf hello-artifact > out/file.txt'\n"
      "    artifacts:\n"
      "      name: bundle\n"
      "      paths: [out]\n");
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id));
  require(record.status == ckgit::CiRunStatus::Success, "the run succeeds");
  require(record.artifacts.size() == 1, "one artifact is recorded");
  require(record.artifacts[0].name == "bundle" && record.artifacts[0].note.empty(),
          "the artifact is stored without a note");
  require(record.artifacts[0].bytes > 0 && record.artifacts[0].sha256.size() == 64,
          "the artifact has a size and a checksum");
  require(record.artifacts[0].expires_epoch_seconds > record.artifacts[0].created_epoch_seconds,
          "an ephemeral artifact expires in the future");
  const auto blob = ckgit::readCiArtifact(fixture.state, "demo", record.run_id, "bundle", 1u << 20);
  require(blob.has_value() && !blob->empty(), "the artifact bundle is downloadable");
  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1 && runs[0].artifacts.size() == 1 && runs[0].artifacts[0].name == "bundle",
          "the run loads with its artifact for the dashboard");
}

void testArtifactOverCap() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: big\n"
      "    steps:\n"
      "      - run: sh -ec 'head -c 100000 /dev/zero > big.bin'\n"
      "    artifacts:\n"
      "      paths: [big.bin]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.artifact_max_bytes = 4096;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "the build still succeeds");
  require(record.artifacts.size() == 1 && !record.artifacts[0].note.empty(),
          "an over-cap artifact is recorded with a note");
  require(record.artifacts[0].bytes == 0, "an over-cap artifact stores no bytes");
  require(!ckgit::readCiArtifact(fixture.state, "demo", record.run_id, "big", 1u << 20).has_value(),
          "an over-cap artifact has no downloadable bundle");
}

void testReleaseOnTag() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p dist && printf installer > dist/app.bin'\n"
      "    artifacts:\n"
      "      name: app\n"
      "      paths: [dist]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.ref = "refs/tags/v1.0.0";
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "the tag build succeeds");
  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1 && runs[0].artifacts.empty(),
          "a tag build's run holds no ephemeral artifacts (they became the release)");
  const auto releases = ckgit::loadReleases(fixture.state, "demo");
  require(releases.size() == 1 && releases[0].tag == "v1.0.0", "the release is recorded under its tag");
  require(releases[0].assets.size() == 1 && releases[0].assets[0].name == "app" &&
              releases[0].assets[0].expires_epoch_seconds == 0,
          "the release asset is durable");
  // D1/WP1: this is the exact dashboard-visible contract that a same-named
  // "release" artifact used to break (a release with no visible assets). Bytes
  // and a well-formed sha256 prove the sidecar was written and not silently
  // overwritten by the release record in the same directory.
  const ckgit::CiArtifactRecord& asset = releases[0].assets[0];
  require(asset.bytes > 0, "the release asset records a non-zero byte count");
  require(asset.sha256.size() == 64 &&
              asset.sha256.find_first_not_of("0123456789abcdef") == std::string::npos,
          "the release asset records a 64-hex sha256");
  const auto blob = ckgit::readCiReleaseAsset(fixture.state, "demo", "v1.0.0", "app", 1u << 20);
  require(blob.has_value() && !blob->empty(), "the release asset is downloadable");
  require(blob->size() == asset.bytes, "the downloaded asset size matches its recorded byte count");
}

// D1/WP1: a workflow that names its artifact "release" is rejected by the
// parser before the runner ever executes a step, so a tag build cannot
// silently produce a release with a clobbered record.
void testReleaseArtifactNamedReleaseRejected() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p dist && printf x > dist/app.bin'\n"
      "    artifacts:\n"
      "      name: release\n"
      "      paths: [dist]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.ref = "refs/tags/v1.0.0";
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Error, "a workflow naming its artifact 'release' errors out");
  require(ckgit::loadReleases(fixture.state, "demo").empty(), "no release is published for the rejected workflow");
}

void testReleaseFailedPublishesNothing() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: sh -ec 'exit 1'\n"
      "    artifacts:\n"
      "      paths: [dist]\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.ref = "refs/tags/v9";
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Failure, "the release build fails");
  require(ckgit::loadReleases(fixture.state, "demo").empty(), "a failed release build publishes nothing");
}

void testPagesPublish() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "pages: { path: public }\n"
      "jobs:\n"
      "  - name: site\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p public && printf site-home > public/index.html'\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.pages_root = fixture.root / "pages";
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "the site build succeeds");
  require(ckgit::currentPagesVersion(opts.pages_root, "demo").has_value(), "a site version is published");
  const auto page = ckgit::readCurrentPage(opts.pages_root, "demo", "", 1u << 20);
  require(page.has_value() && page->content == "site-home", "the published site serves index.html");
}

void testPagesNotPublishedOnFeatureBranch() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "on: { branches: [main, feature] }\n"
      "pages: { path: public }\n"
      "jobs:\n"
      "  - name: site\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p public && printf x > public/index.html'\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  opts.pages_root = fixture.root / "pages";
  opts.ref = "refs/heads/feature";  // a trigger, but not the default branch
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Success, "the feature build succeeds");
  require(!ckgit::currentPagesVersion(opts.pages_root, "demo").has_value(),
          "only the default branch publishes the site");
}

void testPagesPublishFailureFailsRun() {
  RunnerFixture fixture;
  const std::string id = fixture.commit(
      "version: 1\n"
      "pages: { path: public }\n"
      "jobs:\n"
      "  - name: site\n"
      "    steps:\n"
      "      - run: sh -ec 'mkdir -p public && printf x > public/index.html'\n");
  ckgit::CiRunnerOptions opts = fixture.options(id);
  // A pages_root that is a regular file makes publishing throw (its destination
  // cannot be created), so the otherwise-green default-branch build must fail
  // visibly instead of reporting success while the live site stays stale.
  const std::filesystem::path broken = fixture.root / "pages-not-a-dir";
  std::ofstream(broken) << "x";
  opts.pages_root = broken;
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(opts);
  require(record.status == ckgit::CiRunStatus::Error, "a failed Pages publish fails the run");
  require(record.detail.find("failed to publish") != std::string::npos,
          "the run detail explains the swallowed publish failure");
  const auto runs = ckgit::loadCiRuns(fixture.state, "demo");
  require(runs.size() == 1 && runs[0].status == ckgit::CiRunStatus::Error,
          "the failed publish is persisted as a failed run");
}

}  // namespace

void testCiRunner() {
  testSuccess();
  testFailureStops();
  testTimeout();
  testOutputCap();
  testLoopbackInsideSandbox();
  testServiceTreeMasked();
  testCacheStillPersists();
  testSkippedWithoutWorkflow();
  testRunIdReusedFromOptions();
  testCancelledBeforeStart();
  testBranchTrigger();
  testDefaultBranchTrigger();
  testArtifacts();
  testArtifactOverCap();
  testReleaseOnTag();
  testReleaseArtifactNamedReleaseRejected();
  testReleaseFailedPublishesNothing();
  testPagesPublish();
  testPagesNotPublishedOnFeatureBranch();
  testPagesPublishFailureFailsRun();
}
