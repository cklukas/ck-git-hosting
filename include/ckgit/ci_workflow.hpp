// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
//   version: 1                     # required, must be 1
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
// A parser sanity bound on a workflow's retention_days; the runner clamps the
// effective value to the server's configured maximum.
inline constexpr unsigned kMaximumCiRetentionDaysCap = 3650;

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

  bool usesShell() const { return argv.empty(); }
};

// An optional bundle of build outputs a job publishes when all its steps
// succeed. `paths` are relative to the checkout, validated to stay within it;
// `retention_days` of 0 means "use the server default", and the runner clamps
// any value to the server's maximum. `name` defaults to the job's name.
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
};

struct CiWorkflow {
  int version = 1;
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
  std::vector<CiJob> jobs;
  // Optional static site to publish from a successful default-branch build: the
  // directory (relative to the checkout) whose contents become the project's
  // Pages site. Empty means the workflow publishes no site.
  std::optional<std::string> pages_path;
};

// Parses the whole `.ckgit/ci.yml` content. Throws std::length_error when a
// documented bound is exceeded and std::runtime_error when the content is
// outside the supported subset or otherwise malformed. On success every field
// is validated: version == 1, at least one job, unique job names, each job has
// at least one step, and each step has exactly one execution form.
CiWorkflow parseCiWorkflow(std::string_view content);

}  // namespace ckgit
