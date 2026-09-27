# Build artifacts and caches

## Sister projects and build caches

A suite whose parts live in separate repositories can build them together with
no network access, and keep repeat builds fast, by declaring the dependencies
as `sisters` and a compiler cache as a `cache`:

```text
version: 1
sisters:
  - ckmath
  - name: ckvision
    ref: v0.5.0
cache:
  - name: ccache
    env: [CCACHE_DIR]
jobs:
  - name: build
    env: { CCACHE_MAXSIZE: "8G" }
    steps:
      - script: |
          cmake -S . -B build -G Ninja \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCKMATH_DIR="$CKGIT_SISTER_CKMATH" \
            -DCKVISION_DIR="$CKGIT_SISTER_CKVISION"
      - script: cmake --build build
```

Each sister is a read-only snapshot from its own hosted project, placed beside
the checkout, so a build finds `../ckmath` (or `$CKGIT_SISTER_CKMATH`) with no
network and no submodules. The cache is a directory private to this project,
owned by the runner account — on Linux, with the service tree masked (see
[sandbox requirements](ci-runner.md#the-sandbox-and-its-requirements)), the checkout and this project's
own declared caches are the only places a step can write — exported here as
both `CKGIT_CACHE_CCACHE` and `CCACHE_DIR`. Because every run executes at the
same fixed path, ccache's stored objects stay valid from one run to the next:
the first build fills the store and later builds recompile only what changed.
Install `ccache` on the runner host and set `ci_cache_root` for the store to
persist; leave `ci_cache_root` unset and the same workflow still runs, just
without cross-run reuse. The store's size is the
tool's to manage (`CCACHE_MAXSIZE` above).

## Build artifacts and retention

A job's `artifacts:` block collects build outputs when the job succeeds. The
runner packs the declared paths into a single `<name>.tar` bundle stored beside
the run, checksums it, and shows it on the CI run page with a download link. A
bundle over `ci_artifact_max_bytes` is dropped and the run notes why; the build
still counts as successful.

Artifacts are **ephemeral** and bounded three ways so a busy project cannot fill
the disk:

- **Time** — each artifact expires after `ci_artifact_retention_days` (default
  7), or the workflow's own `retention_days`, capped by
  `ci_artifact_max_retention_days`.
- **Budget** — when a project exceeds `ci_artifact_max_project_bytes` or the
  server exceeds `ci_artifact_max_total_bytes`, the oldest artifacts are evicted
  until under budget.
- **Run history** — only the newest `ci_runs_keep` run directories per project
  are kept; older ones (and their artifacts and logs) are pruned.

With `ci_artifact_keep_latest` (default on), each project's newest run's
artifacts are never removed by the timer or the budget, so the current build is
always downloadable. The runner enforces all of this in a sweep every
`ci_cleanup_interval_seconds`. Download a bundle from the run page or directly:

```text
curl -O http://<server>:<http_port>/project/myproject/ci/<run-id>/artifacts/<name>
```

From a paired device, `ckgit ci artifacts RUN` lists a run's bundles and
`ckgit ci download RUN NAME` fetches one exactly like `ckgit release download`
(see [releases](releases.md)): through a short-lived tunnel over your ordinary SSH
login, verified against the listed size and checksum before it is written.

Durable release assets, attached to a tag, never expire; see [releases](releases.md).
