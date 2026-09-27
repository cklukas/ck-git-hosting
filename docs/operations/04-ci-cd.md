# Run your first CI workflow

Add a `.ckgit/ci.yml` workflow to a hosted project to build and test every push.
Results, logs, and artifacts appear in the dashboard and in `ckgit ci`.

The server administrator first [configures the CI runner](ci-runner.md).

## Enable CI for a project

Opt in per project with `ckgit-admin` on the server. It must run as the `ckgit`
service account, which owns the private state root (plain `sudo` runs it as
root, which the state root's ownership check refuses), and it needs the
configuration file to find that state root:

```text
sudo -u ckgit ckgit-admin ci enable myproject --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin ci status myproject --config /etc/ck-git-hosting/server.ini
```

`ci status` prints `myproject: CI enabled` or `myproject: CI disabled`. Use
`ci disable` to turn it off again; disabling stops new runs but keeps existing
run history. Removing the project also clears its CI opt-in and records.
Enabling and disabling CI are administrator actions: a paired device can see
whether CI is on (`ckgit ci status`) but cannot change it.

## Write `.ckgit/ci.yml`

Commit the workflow at `.ckgit/ci.yml` in the repository. It is read from the
pushed commit, so a build always matches the commit that triggered it. This
section gets a first workflow running; the
[`.ckgit/ci.yml` reference](06-ci-yml-reference.md) has every accepted and
rejected syntax construct, the complete schema and bounds, the trigger
matrix, and the exact sandbox and environment a step runs in.

```text
version: 1
on: { branches: [main] }
env: { BUILD_ROOT: /tmp/ck }
jobs:
  - name: build
    steps:
      - run: [make, all]
      - name: tests
        run: make check
      - script: |
          make docs
    artifacts:
      name: build
      paths: [dist/, build/app.bin]
      retention_days: 14
```

- `version:` must be `1`.
- `on: { branches: [...] }` and `on: { tags: [...] }` choose which branches and
  tags trigger the workflow (a tag pattern is an exact name or a trailing `*`,
  such as `v*`). Omit `on:` entirely to build the default branch and cut a
  release on any tag. A non-triggering ref is recorded `skipped`, as is a commit
  with no `.ckgit/ci.yml`.
- `env:` is fixed `key: value` pairs; there is no `$VAR` expansion. A job's
  `env:` is merged over the top-level `env:`.
- A step's `run:` written as a **list** is an exact command with no shell. A
  `run:` **scalar** or a `script:` block runs with `sh -ec` inside the sandbox.
- `artifacts:` (optional, per job) packs the named `paths` into one bundle after
  the job's steps succeed. `paths` are relative to the checkout and may not be
  absolute or contain `..`; `name` defaults to the job name and may not be
  `release`, which names the release record a tag build writes beside each
  asset's own sidecar; `retention_days` overrides the server default and is
  clamped to its maximum.
- `sisters:` lists other projects hosted on this server whose source the build
  needs. Each is materialised read-only beside the checkout — reachable as
  `../<name>` and exported as `CKGIT_SISTER_<NAME>` — from its default branch, or
  a branch, tag, or commit you pin. No network is used and only same-server
  projects are allowed. Write `- name` for the default branch, or the two-line
  form below it to pin. See below.
- `cache:` lists persistent build caches (e.g. a ccache store) kept across runs.
  Each is a directory exported as `CKGIT_CACHE_<NAME>`, plus any variables bound
  with `env:`. Persistence needs `ci_cache_root` set on the server; without it a
  declared cache still works, only per-run, so a workflow is portable. See below.

The format is a strict, bounded subset of YAML — not GitHub Actions. Unknown
keys, tabs for indentation, wrong types, or anything past the documented size
limits are rejected, and the run is recorded `error` with the reason. Keep a
workflow well under 64 KiB, 64 jobs, and 128 steps per job.

Check a workflow before pushing it: `ckgit ci lint` parses `.ckgit/ci.yml`
from the working tree (or a file, or a commit with `--rev REV`) with the
runner's own parser, and prints a summary of its triggers and jobs or the
first error with its line. It works offline, so it cannot know the server's
default branch, whether sister projects exist, or the server's limits.

## Next steps

- [Read results, follow logs, and cancel a run](../guides/watch-ci.md)
- [Keep artifacts and reuse build caches](ci-artifacts-and-cache.md)
- [Create a release](releases.md)
- [Publish a project website](pages-hosting.md)
- [Configure the runner and sandbox](ci-runner.md)
- [Workflow reference and troubleshooting](06-ci-yml-reference.md)
