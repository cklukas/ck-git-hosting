// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_workflow.hpp"

#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci workflow: " + message);
}

bool rejected(const std::string& text) {
  try {
    ckgit::parseCiWorkflow(text);
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

void testValidWorkflow() {
  const std::string text =
      "version: 1\n"
      "on: { branches: [main, release] }\n"
      "env: { BUILD_ROOT: /tmp/ck, CC: clang }\n"
      "jobs:\n"
      "  - name: build\n"
      "    env: { PROFILE: release }\n"
      "    steps:\n"
      "      - run: [make, all]\n"
      "      - name: tests\n"
      "        run: make check\n"
      "      - script: |\n"
      "          make docs\n"
      "          echo done\n"
      "  - name: lint\n"
      "    steps:\n"
      "      - run: [make, lint]\n";
  const ckgit::CiWorkflow workflow = ckgit::parseCiWorkflow(text);
  require(workflow.version == 1, "version parsed");
  require(workflow.branches.size() == 2 && workflow.branches[0] == "main" && workflow.branches[1] == "release",
          "branches parsed");
  require(workflow.env.size() == 2 && workflow.env[0].first == "BUILD_ROOT" &&
              workflow.env[0].second == "/tmp/ck" && workflow.env[1].first == "CC",
          "top-level env parsed in order");
  require(workflow.jobs.size() == 2, "two jobs");

  const ckgit::CiJob& build = workflow.jobs[0];
  require(build.name == "build", "job name");
  require(build.env.size() == 1 && build.env[0].first == "PROFILE" && build.env[0].second == "release",
          "per-job env");
  require(build.steps.size() == 3, "three steps");

  require(!build.steps[0].usesShell() && build.steps[0].argv.size() == 2 &&
              build.steps[0].argv[0] == "make" && build.steps[0].argv[1] == "all",
          "argv step is exec form");
  require(build.steps[1].name == "tests" && build.steps[1].usesShell() &&
              build.steps[1].script == "make check",
          "scalar run is a shell command");
  require(build.steps[2].usesShell() && build.steps[2].script == "make docs\necho done\n",
          "block script preserved with trailing newline");

  require(workflow.jobs[1].name == "lint" && workflow.jobs[1].steps.size() == 1, "second job");
}

void testBlockStyleAndComments() {
  const std::string text =
      "# a leading comment\n"
      "version: 1   # trailing comment\n"
      "on:\n"
      "  branches:\n"
      "    - main\n"
      "jobs:\n"
      "  - name: only\n"
      "    steps:\n"
      "      - run: [echo, hi]  # inline note\n";
  const ckgit::CiWorkflow workflow = ckgit::parseCiWorkflow(text);
  require(workflow.branches.size() == 1 && workflow.branches[0] == "main", "block branches list");
  require(workflow.jobs.size() == 1 && workflow.jobs[0].steps.size() == 1, "block job");
  require(workflow.jobs[0].steps[0].argv.size() == 2 && workflow.jobs[0].steps[0].argv[1] == "hi",
          "trailing comment stripped from a flow list");
}

void testQuoting() {
  const std::string text =
      "version: 1\n"
      "env: { GREETING: \"a # b\", LITERAL: 'it''s' }\n"
      "jobs:\n"
      "  - name: q\n"
      "    steps:\n"
      "      - run: \"echo hi\"\n";
  const ckgit::CiWorkflow workflow = ckgit::parseCiWorkflow(text);
  require(workflow.env.size() == 2 && workflow.env[0].second == "a # b", "double quotes keep the hash");
  require(workflow.env[1].second == "it's", "single-quote doubling");
  require(workflow.jobs[0].steps[0].usesShell() && workflow.jobs[0].steps[0].script == "echo hi",
          "quoted scalar run");
}

void testRejections() {
  require(rejected(""), "empty file");
  require(rejected("jobs:\n  - name: a\n    steps:\n      - run: [x]\n"), "missing version");
  require(rejected("version: 2\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"), "wrong version");
  require(rejected("version: 1\n"), "missing jobs");
  require(rejected("version: 1\njobs: []\n"), "empty jobs");
  require(rejected("version: 1\njobs:\n  - steps:\n      - run: [x]\n"), "job without a name");
  require(rejected("version: 1\njobs:\n  - name: a\n"), "job without steps");
  require(rejected("version: 1\njobs:\n  - name: a\n    steps: []\n"), "empty steps");
  require(rejected("version: 1\njobs:\n  - name: a\n    steps:\n      - name: only\n"),
          "step with neither run nor script");
  require(rejected("version: 1\njobs:\n  - name: a\n    steps:\n      - run: make\n        script: |\n          x\n"),
          "step with both run and script");
  require(rejected("version: 1\njobs:\n  - name: a\n    steps:\n      - run: []\n"), "empty argv list");
  require(rejected("version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n  - name: a\n    steps:\n      - run: [y]\n"),
          "duplicate job name");
  require(rejected("version: 1\nversion: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"),
          "duplicate top-level key");
  require(rejected("version: 1\nsurprise: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"),
          "unknown top-level key");
  require(rejected("version: 1\nenv: { 1bad: x }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"),
          "invalid env name");
  require(rejected("version: 1\non: { branches: ['bad branch'] }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"),
          "invalid branch name");
  require(rejected("version: 1\njobs:\n\t- name: a\n    steps:\n      - run: [x]\n"), "tab indentation");
  require(rejected("version: 1\r\njobs:\r\n  - name: a\r\n"), "carriage returns");
  require(rejected(std::string("version: 1\nenv: { X: \"") + std::string(3, static_cast<char>(0xff)) + "\" }\n"),
          "invalid utf-8");
}

void testBounds() {
  bool threw_length = false;
  try {
    ckgit::parseCiWorkflow(std::string(ckgit::kMaximumCiWorkflowBytes + 1, 'x'));
  } catch (const std::length_error&) {
    threw_length = true;
  } catch (const std::exception&) {
  }
  require(threw_length, "oversize file throws length_error");

  std::string many = "version: 1\njobs:\n";
  for (std::size_t index = 0; index <= ckgit::kMaximumCiJobs; ++index) {
    many += "  - name: j" + std::to_string(index) + "\n    steps:\n      - run: [x]\n";
  }
  require(rejected(many), "too many jobs");
}

}  // namespace

void testCiWorkflow() {
  testValidWorkflow();
  testBlockStyleAndComments();
  testQuoting();
  testRejections();
  testBounds();
}
