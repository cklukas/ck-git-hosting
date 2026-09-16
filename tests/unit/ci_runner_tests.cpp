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

#include "ckgit/ci_store.hpp"
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
  require(record.status == ckgit::CiRunStatus::Success, "a passing workflow succeeds");
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

void testSkippedWithoutWorkflow() {
  RunnerFixture fixture;
  const std::string id = fixture.commit("just a readme\n", "README.md");
  const ckgit::CiRunRecord record = ckgit::runCiWorkflow(fixture.options(id));
  require(record.status == ckgit::CiRunStatus::Skipped, "a commit with no workflow is skipped");
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

}  // namespace

void testCiRunner() {
  testSuccess();
  testFailureStops();
  testTimeout();
  testOutputCap();
  testSkippedWithoutWorkflow();
  testBranchTrigger();
}
