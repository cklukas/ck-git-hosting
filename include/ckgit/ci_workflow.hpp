// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/yaml_subset.hpp"

namespace ckgit {

// A CI workflow parsed from a repository's `.ckgit/ci.yml`.
//
// The file format is a deliberately small, strict subset of YAML — enough to
// express "on these branches, run these jobs, each a list of steps" and no
// more. It is NOT GitHub Actions: there is no `uses:`, no marketplace, no
// `${{ }}` expression language, and no implicit type coercion. The parser is
// bounded and fail-closed in the same spirit as the INI metadata store: any
// construct it does not explicitly understand is rejected rather than guessed.
//
// The supported subset, by example:
//
//   version: 1                     # required; 1 = sequential, 2 = distributed
//   on: { branches: [main] }       # optional; default = the repo default branch
//   env: { BUILD_ROOT: /tmp/ck }   # optional; fixed key=value, no interpolation
//   jobs:                          # required, non-empty, run in listed order
//     - name: build                # required, unique per file
//       env: { CC: clang }         # optional, merged over the top-level env
//       steps:                     # required, non-empty
//         - run: [make, all]       # a list is an exact argv, run without a shell
//         - name: tests            # an optional step label
//           run: make check        # a scalar is run with `sh -ec`
//         - script: |              # a block scalar is a multi-line `sh -ec` script
//             make docs
//
// Version 2 additionally accepts per-job runs_on and needs (a list of job
// names), and a shell on script steps (sh, powershell, pwsh). Top-level
// runs_on supplies job defaults. Empty needs means independent execution;
// unknown dependencies, self dependencies and cycles are rejected.
//
// Both flow (`{a: b}`, `[a, b]`) and block (`key:` / `- item`) styles are
// accepted for mappings and sequences; scalars may be plain, single-quoted, or
// double-quoted; `|` introduces a block literal. Indentation is spaces only.

// Bounds. Public so tests and the fuzz target can reference them, and so a
// caller can report the limit that a rejected file exceeded. A file, count, or
// length beyond these throws std::length_error; any other malformed input
// throws std::runtime_error.
inline constexpr std::size_t kMaximumCiWorkflowBytes = 64 * 1024;
inline constexpr std::size_t kMaximumCiLineBytes = 4096;
inline constexpr std::size_t kMaximumCiLines = 4096;
inline constexpr std::size_t kMaximumCiJobs = 64;
inline constexpr std::size_t kMaximumCiStepsPerJob = 128;
inline constexpr std::size_t kMaximumCiEnvEntries = 64;
inline constexpr std::size_t kMaximumCiBranches = 64;
inline constexpr std::size_t kMaximumCiArgvItems = 64;
inline constexpr std::size_t kMaximumCiNameBytes = 64;
inline constexpr std::size_t kMaximumCiKeyBytes = 128;
inline constexpr std::size_t kMaximumCiScalarBytes = 4096;
inline constexpr std::size_t kMaximumCiScriptBytes = 16 * 1024;
inline constexpr std::size_t kMaximumCiArtifactPaths = 64;
inline constexpr std::size_t kMaximumCiPathBytes = 1024;
inline constexpr std::size_t kMaximumCiSisters = 16;
inline constexpr std::size_t kMaximumCiCaches = 8;
inline constexpr std::size_t kMaximumCiCacheEnv = 8;
// A parser sanity bound on a workflow's retention_days; the runner clamps the
// effective value to the server's configured maximum.
inline constexpr unsigned kMaximumCiRetentionDaysCap = 3650;
// The deepest finite history a checkout may ask for (0 means full history).
inline constexpr unsigned kMaximumCiCheckoutDepth = 100000;

// How the shared YAML subset is bounded and named when it reads a workflow:
// the limits above, applied by the parser, plus the "ci workflow: ..." wording
// of every error. A block sequence may hold as many items as the larger of the
// job and step limits; a mapping may hold the env limit plus the schema's own
// keys; the tree may nest 16 deep, more than the schema ever needs.
inline constexpr YamlBounds kCiWorkflowYamlBounds{
    kMaximumCiWorkflowBytes,
    kMaximumCiLineBytes,
    kMaximumCiLines,
    kMaximumCiKeyBytes,
    kMaximumCiScalarBytes,
    kMaximumCiScriptBytes,
    kMaximumCiArgvItems,
    kMaximumCiJobs > kMaximumCiStepsPerJob ? kMaximumCiJobs : kMaximumCiStepsPerJob,
    kMaximumCiEnvEntries + 8,
    16,
};
inline constexpr YamlDialect kCiWorkflowYamlDialect{kCiWorkflowYamlBounds, "ci workflow", "the workflow"};

// One environment binding, kept in file order. Values are literal: the runner
// never expands `$VAR` or any other reference when applying them.
using CiEnv = std::vector<std::pair<std::string, std::string>>;

// One step of a job. Exactly one execution form is set:
//   - `argv` non-empty: run this exact argument vector, without a shell.
//   - `script` non-empty: run this text with `sh -ec` inside the sandbox.
struct CiStep {
  std::string name;               // optional label; may be empty
  std::vector<std::string> argv;  // set for the list `run:` form
  std::string script;             // set for the scalar `run:` / block `script:` form

  // Version 2 only: empty selects the runner platform default.
  std::string shell;  // sh, powershell, or pwsh; illegal with exact argv

  bool usesShell() const { return argv.empty(); }
};

// An optional bundle of build outputs a job publishes when all its steps
// succeed. `paths` are relative to the checkout, validated to stay within it;
// `retention_days` of 0 means "use the server default", and the runner clamps
// any value to the server's maximum. `name` defaults to the job's name, and
// may not be "release" (reserved for the tag-build release record; see
// ci_store.hpp).
struct CiArtifact {
  std::string name;
  std::vector<std::string> paths;
  unsigned retention_days = 0;
};

struct CiJob {
  std::string name;
  CiEnv env;
  std::vector<CiStep> steps;
  std::optional<CiArtifact> artifact;
  // Version 2: independently placed jobs; empty needs means runnable immediately.
  std::vector<std::string> runs_on;
  std::vector<std::string> needs;
};

// How much Git history a checkout carries. Every checkout is a shallow Git
// clone of exactly the commit under build, `depth` commits deep (1 = only that
// commit, 0 = the full history), or with `since` set, every commit since that
// date (YYYY-MM-DD) instead. `tags` also fetches the tags inside that history,
// for `git describe`. `archive` drops the .git directory afterwards: the plain
// tree a runner produced before checkouts were Git clones.
struct CiCheckout {
  unsigned depth = 1;
  std::string since;
  bool tags = false;
  bool archive = false;
};

// One sister project a workflow depends on: another project hosted on this same
// server whose source is materialised beside the checkout. `ref` pins it to a
// branch, tag, or commit; empty means the project's current default branch.
// The project may name itself, but only with a `ref` (another of its branches).
// `checkout` sets how much of the sister's history comes along (its `archive`
// is never set: a sister is always a Git clone like the main checkout).
struct CiSister {
  std::string name;
  std::string ref;  // empty = default-branch HEAD
  CiCheckout checkout;
};

// One build cache a workflow keeps across runs: a persistent, project-scoped
// directory (e.g. a ccache store). The runner exports its path as
// CKGIT_CACHE_<NAME>, plus any environment variables named in `env` (e.g.
// CCACHE_DIR), to every step.
struct CiCache {
  std::string name;
  std::vector<std::string> env;
};

struct CiWorkflow {
  int version = 1;
  // New branch pushes supersede older queued/running jobs of the same ref.
  bool auto_cancel = true;
  // Trigger sets, resolved from the optional `on:` block:
  //   - no `on:` at all  -> triggers_default_branch = true, tags = ["*"]
  //     (build the repo's default branch, and cut a release on any tag)
  //   - `on: { branches: [...] }` -> those branches; tags only if listed too
  //   - `on: { tags: [...] }`     -> those tag patterns; branches only if listed
  // A tag pattern is an exact name or a trailing-'*' prefix (e.g. "v*"). The
  // caller resolves the repository's default branch; the file never names one.
  std::vector<std::string> branches;
  std::vector<std::string> tags;
  bool triggers_default_branch = false;
  CiEnv env;
  // Other projects hosted on this same server whose source this build needs
  // (e.g. a shared library checked out beside the repository). The runner
  // materialises each one's snapshot, read-only, next to the checkout so a
  // build finds it at `../<name>`, and exports its absolute path as
  // `CKGIT_SISTER_<NAME>`. Nothing leaves the server and no network is used —
  // see runCiWorkflow. Empty means none.
  std::vector<CiSister> sisters;
  // How the commit under build is checked out (see CiCheckout); the default is
  // a depth-1 Git clone.
  CiCheckout checkout;
  // `runs_on:` -- the runner labels this workflow needs (`runs_on: macos` or
  // `runs_on: [arm64, big-disk]`): only a runner carrying every one of them
  // (a runner's name counts as a label) takes its jobs. Empty: any runner.
  std::vector<std::string> runs_on;
  // Persistent build caches this workflow keeps across runs. See CiCache and
  // runCiWorkflow.
  std::vector<CiCache> caches;
  std::vector<CiJob> jobs;
  // Optional static site to publish from a successful default-branch build: the
  // directory (relative to the checkout) whose contents become the project's
  // Pages site. Empty means the workflow publishes no site.
  std::optional<std::string> pages_path;
  std::string pages_job;  // required Pages producer in version 2; empty in version 1
};

// Parses the whole `.ckgit/ci.yml` content. Throws std::length_error when a
// documented bound is exceeded and std::runtime_error when the content is
// outside the supported subset or otherwise malformed. On success every field
// is validated: version is 1 or 2, at least one job, unique job names, each job has
// at least one step, and each step has exactly one execution form.
CiWorkflow parseCiWorkflow(std::string_view content);

// Reduce a validated version-2 workflow to precisely the assigned job.
// The original workflow text remains authoritative for wire transport.
CiWorkflow selectCiWorkflowJob(CiWorkflow workflow, std::string_view name);

}  // namespace ckgit
