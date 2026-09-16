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

void testArtifacts() {
  const std::string text =
      "version: 1\n"
      "jobs:\n"
      "  - name: build\n"
      "    steps:\n"
      "      - run: [make, all]\n"
      "    artifacts:\n"
      "      name: linux\n"
      "      paths: [dist/, build/app.bin]\n"
      "      retention_days: 14\n"
      "  - name: nokeep\n"
      "    steps:\n"
      "      - run: [true]\n";
  const ckgit::CiWorkflow workflow = ckgit::parseCiWorkflow(text);
  require(workflow.jobs.size() == 2, "two jobs");
  require(workflow.jobs[0].artifact.has_value(), "first job declares an artifact");
  const ckgit::CiArtifact& artifact = *workflow.jobs[0].artifact;
  require(artifact.name == "linux", "artifact name parsed");
  require(artifact.paths.size() == 2 && artifact.paths[0] == "dist/" && artifact.paths[1] == "build/app.bin",
          "artifact paths parsed in order");
  require(artifact.retention_days == 14, "retention_days parsed");
  require(!workflow.jobs[1].artifact.has_value(), "a job without artifacts has none");

  // The artifact name defaults to the job name when omitted.
  const ckgit::CiWorkflow defaulted = ckgit::parseCiWorkflow(
      "version: 1\njobs:\n  - name: pack\n    steps:\n      - run: [true]\n    artifacts:\n      paths: [out]\n");
  require(defaulted.jobs[0].artifact.has_value() && defaulted.jobs[0].artifact->name == "pack",
          "artifact name defaults to the job name");

  // Unsafe paths and bad shapes are rejected.
  const std::string base = "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n    artifacts:\n";
  require(rejected(base + "      paths: ['../escape']\n"), "parent traversal path");
  require(rejected(base + "      paths: ['/etc/passwd']\n"), "absolute path");
  require(rejected(base + "      paths: []\n"), "empty paths list");
  require(rejected(base + "      name: bad name\n      paths: [out]\n"), "invalid artifact name");
  require(rejected(base + "      paths: [out]\n      retention_days: 0\n"), "zero retention");
  require(rejected(base + "      paths: [out]\n      surprise: 1\n"), "unknown artifact key");
  require(rejected(base + "      retention_days: 3\n"), "artifacts without paths");
}

void testTagTriggers() {
  const ckgit::CiWorkflow none =
      ckgit::parseCiWorkflow("version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  require(none.triggers_default_branch, "no on: triggers the default branch");
  require(none.branches.empty(), "no on: lists no explicit branches");
  require(none.tags.size() == 1 && none.tags[0] == "*", "no on: releases on any tag");

  const ckgit::CiWorkflow tags = ckgit::parseCiWorkflow(
      "version: 1\non: { tags: [v*, release-1] }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  require(!tags.triggers_default_branch, "an explicit on: does not add the default branch");
  require(tags.branches.empty(), "tags-only lists no branches");
  require(tags.tags.size() == 2 && tags.tags[0] == "v*" && tags.tags[1] == "release-1", "tag patterns parse");

  const ckgit::CiWorkflow branch = ckgit::parseCiWorkflow(
      "version: 1\non: { branches: [main] }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  require(branch.branches.size() == 1 && branch.tags.empty() && !branch.triggers_default_branch,
          "branches-only triggers no tags");

  require(rejected("version: 1\non: { tags: ['bad tag'] }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n"),
          "an invalid tag pattern is rejected");
}

void testPagesWorkflow() {
  const ckgit::CiWorkflow with = ckgit::parseCiWorkflow(
      "version: 1\npages: { path: public }\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  require(with.pages_path.has_value() && *with.pages_path == "public", "pages path parses");
  const ckgit::CiWorkflow without =
      ckgit::parseCiWorkflow("version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n");
  require(!without.pages_path.has_value(), "no pages by default");
  const std::string base = "version: 1\njobs:\n  - name: a\n    steps:\n      - run: [x]\n";
  require(rejected("version: 1\npages: { path: '../x' }\n" + base.substr(11)), "unsafe pages path rejected");
  require(rejected("version: 1\npages: { path: /abs }\n" + base.substr(11)), "absolute pages path rejected");
  require(rejected("version: 1\npages: { path: public, extra: 1 }\n" + base.substr(11)), "unknown pages key rejected");
  require(rejected("version: 1\npages: {}\n" + base.substr(11)), "pages without a path rejected");
}

void testSisters() {
  const std::string base = "\njobs:\n  - name: a\n    steps:\n      - run: [x]\n";
  // A flow list of bare project names.
  const ckgit::CiWorkflow flow = ckgit::parseCiWorkflow("version: 1\nsisters: [ckmath, cgrapher]" + base);
  require(flow.sisters.size() == 2, "two sisters parse");
  require(flow.sisters[0].name == "ckmath" && flow.sisters[0].ref.empty(), "a bare sister has no ref");
  require(flow.sisters[1].name == "cgrapher", "second sister name");
  // A block list mixing a bare name and a pinned { name, ref } mapping.
  const ckgit::CiWorkflow pinned = ckgit::parseCiWorkflow(
      "version: 1\n"
      "sisters:\n"
      "  - ckmath\n"
      "  - name: cgrapher\n"
      "    ref: v1.2.0\n" +
      base);
  require(pinned.sisters.size() == 2, "mixed sisters parse");
  require(pinned.sisters[0].name == "ckmath" && pinned.sisters[0].ref.empty(), "unpinned sister");
  require(pinned.sisters[1].name == "cgrapher" && pinned.sisters[1].ref == "v1.2.0", "pinned sister ref");
  // A commit hash and a branch name are both accepted refs.
  const ckgit::CiWorkflow refs = ckgit::parseCiWorkflow(
      "version: 1\n"
      "sisters:\n"
      "  - name: ckmath\n"
      "    ref: 0123abcd\n"
      "  - name: cgrapher\n"
      "    ref: feature/x\n" +
      base);
  require(refs.sisters[0].ref == "0123abcd" && refs.sisters[1].ref == "feature/x", "hash and branch refs");
  require(ckgit::parseCiWorkflow("version: 1" + base).sisters.empty(), "no sisters by default");
  require(rejected("version: 1\nsisters: [ '../evil' ]" + base), "unsafe sister name rejected");
  require(rejected("version: 1\nsisters: [ ckmath, ckmath ]" + base), "duplicate sister rejected");
  require(rejected("version: 1\nsisters:\n  - ref: v1\n" + base), "sister without a name rejected");
  require(rejected("version: 1\nsisters:\n  - name: ckmath\n    bogus: 1\n" + base), "unknown sister key rejected");
  require(rejected("version: 1\nsisters:\n  - name: ckmath\n    ref: '-x'\n" + base), "option-like ref rejected");
  require(rejected("version: 1\nsisters:\n  - name: ckmath\n    ref: 'a..b'\n" + base), "range ref rejected");
}

void testCaches() {
  const std::string base = "\njobs:\n  - name: a\n    steps:\n      - run: [x]\n";
  // A flow list of bare cache names.
  const ckgit::CiWorkflow flow = ckgit::parseCiWorkflow("version: 1\ncache: [ccache, pip]" + base);
  require(flow.caches.size() == 2, "two caches parse");
  require(flow.caches[0].name == "ccache" && flow.caches[0].env.empty(), "a bare cache binds no env");
  // A block list mixing a bare name and a { name, env } mapping.
  const ckgit::CiWorkflow bound = ckgit::parseCiWorkflow(
      "version: 1\n"
      "cache:\n"
      "  - ccache\n"
      "  - name: pip\n"
      "    env: [PIP_CACHE_DIR, XDG_CACHE_HOME]\n" +
      base);
  require(bound.caches.size() == 2, "mixed caches parse");
  require(bound.caches[1].name == "pip" && bound.caches[1].env.size() == 2, "cache env bindings parse");
  require(bound.caches[1].env[0] == "PIP_CACHE_DIR", "first bound variable");
  require(ckgit::parseCiWorkflow("version: 1" + base).caches.empty(), "no caches by default");
  require(rejected("version: 1\ncache: [ '../bad' ]" + base), "unsafe cache name rejected");
  require(rejected("version: 1\ncache: [ ccache, ccache ]" + base), "duplicate cache rejected");
  require(rejected("version: 1\ncache:\n  - name: x\n    env: [ '1bad' ]\n" + base), "invalid env name rejected");
  require(rejected("version: 1\ncache:\n  - env: [X]\n" + base), "cache without a name rejected");
  require(rejected("version: 1\ncache:\n  - name: x\n    bogus: 1\n" + base), "unknown cache key rejected");
}

}  // namespace

void testCiWorkflow() {
  testValidWorkflow();
  testBlockStyleAndComments();
  testQuoting();
  testArtifacts();
  testTagTriggers();
  testPagesWorkflow();
  testSisters();
  testCaches();
  testRejections();
  testBounds();
}
