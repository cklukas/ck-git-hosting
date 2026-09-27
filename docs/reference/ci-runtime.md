# CI execution environment

## Execution

Jobs run in the order they are listed in the file; within a job, steps run
in the order listed. The first step that fails — non-zero exit, a timeout,
or a cancellation — stops the **entire run**, not just the current job; jobs
after it never start.

A step is exactly one of two execution forms:

- **`run:` as a list** is an exact argument vector, executed with
  `execvp` and **no shell at all** — no globbing, no `$VAR` expansion, no
  pipelines. `run: [make, all]` runs the `make` binary found on `PATH` with
  the single argument `all`.
- **`run:` as a scalar, or `script:`** (almost always a `|` block for
  anything beyond one line) is executed as `sh -ec '<text>'` inside the
  sandbox. The shell is deliberate here — the sandbox is the isolation
  boundary, not the absence of a shell — and this is the only place `$VAR`
  expansion, pipelines, or multiple commands are available.

A step's stdin is always `/dev/null`; it never waits on input. Its stdout
and stderr are combined into one interleaved stream, captured up to
`ci_max_log_bytes` (default 1 MiB / 1048576 bytes) — output past the cap is
discarded, not buffered, and the run record notes that the log was
truncated. Each step has its own wall-clock budget, `ci_timeout_seconds`
(default 1800 seconds / 30 minutes); on expiry the step's whole process
group is killed.

Exit-code and status mapping, checked in this order per step:

1. The run was cancelled (an administrator requested it) → status
   `cancelled`.
2. The step could not even be started (e.g. the interpreter is missing) →
   status `error`.
3. The step exceeded its timeout → status `timeout`.
4. The step exited non-zero → status `failure`, with the exact exit code
   recorded (a signal-terminated step is recorded as `128 + signal number`,
   matching the ordinary shell convention).
5. Otherwise the step succeeded and the run continues to the next one.

A timed-out or cancelled step's own exit code is not meaningful (it was
killed, not exited) and is recorded as `-1`. When every job's every step
succeeds, the run's overall status is `success`.

## Environment

Every step runs with exactly these variables, before anything the workflow
itself adds:

| Variable | Value |
|---|---|
| `PATH` | `/usr/local/bin:/usr/bin:/bin` |
| `HOME` | the sandbox home directory (`/mnt/home`; see [§7](#filesystem)) |
| `TMPDIR` | the sandbox temp directory (`/mnt/tmp`) |
| `LANG` | `C` |
| `LC_ALL` | `C` |
| `CI` | `true` |
| `CKGIT_CI` | `1` |
| `CKGIT_COMMIT` | the full commit id under build |
| `CKGIT_REF` | the pushed ref, e.g. `refs/heads/master` or `refs/tags/v1.0.0` |

`CKGIT_CI` (not the generic `CI`) is the one to test for "am I running
inside this project's own sandbox" specifically — a test that needs to skip
a check only when self-hosted CI is running it nested inside itself, for
example, should check `CKGIT_CI`, since a GitHub Actions runner also sets
`CI=true` but never sets `CKGIT_CI`.

**Precedence, lowest to highest** — a later layer overwrites a same-named
variable from an earlier one:

1. The defaults above.
2. The workflow's top-level `env:`.
3. The running job's `env:`, merged over the top-level `env:`.
4. Server/operator-level variables configured outside the workflow file
   (not something `.ckgit/ci.yml` itself controls).
5. Sister and cache exports (below) — **last, and unshadowable**: nothing
   in the workflow's own `env:` can override a `CKGIT_SISTER_<NAME>` or
   `CKGIT_CACHE_<NAME>` export, or a cache's own bound variable names.

A workflow's `env:`/job `env:` *can* override any of the five built-in
defaults, including `PATH` and even `CKGIT_COMMIT`/`CKGIT_REF` themselves —
there is no protection against that. Values are always taken literally; the
runner never expands `$VAR` or any other reference inside an `env:` value.

**Sister and cache variable names** follow one name-mangling rule: the
fixed prefix (`CKGIT_SISTER_` or `CKGIT_CACHE_`), then every byte of the
declared name is uppercased if it is alphanumeric, or replaced with `_`
otherwise. A sister named `ck-vision.core` is exported as
`CKGIT_SISTER_CK_VISION_CORE`; a cache named `ccache` is exported as
`CKGIT_CACHE_CCACHE`. A `cache[].env` entry binds that same path under its
own literal name too, unmangled — `env: [CCACHE_DIR]` additionally exports
`CCACHE_DIR` pointing at the same directory as `CKGIT_CACHE_CCACHE`.

## Filesystem

On Linux, with unprivileged user namespaces available, each step runs
inside its own user, mount, and (unless `ci_allow_network=true`) network
namespace, on a throwaway checkout under `/mnt`:

| Path | Contents |
|---|---|
| `/mnt/src` | the checkout — also the step's working directory |
| `/mnt/home` | `HOME`; always created, sandboxed or not |
| `/mnt/tmp` | `TMPDIR`; always created, sandboxed or not |
| `/mnt/<sister-name>` | each declared sister's read-only source, sibling of `src` — reachable from inside the checkout as `../<sister-name>` |
| `/mnt/.cache/<cache-name>` | each declared cache, whether persisted across runs (`ci_cache_root` configured) or ephemeral (this run only) |

**What gets masked.** The step's mount namespace also covers every one of
the following, when configured, with an empty, private, mode-0700 tmpfs
capped at 64 KiB (65536 bytes) — the step cannot see into them at all, not
even to confirm they exist: the private state root (other projects' CI
records, run spool, and release metadata), the CI build root (other runs'
scratch trees), the cache root (other projects' persistent caches — this
project's own is exempted, since it is bound in before the mask is applied),
the Pages storage root, and the directory containing every hosted project's
bare repository. A path that does not exist is silently skipped rather than
failing the step.

**Networking.** Unless `ci_allow_network=true`, the step gets its own
network namespace with only loopback (`127.0.0.1`/`::1`) brought up — no
LAN, no outbound internet, nothing else reachable. `ci_allow_network=true`
does not hand the step an isolated network namespace with its own
interfaces; it **skips creating a network namespace at all**, so the step
sees the host's real interfaces and the LAN directly, exactly like any other
process on the machine. Only turn it on for a project whose steps you would
already trust with ordinary host network access.

**Resource limits.** Exactly two `setrlimit` calls, nothing else: no core
dumps (`RLIMIT_CORE=0`), and no single file over 4 GiB (`RLIMIT_FSIZE`).
There is no memory limit and no CPU-time limit beyond the step's wall-clock
timeout — the timeout and the output-byte cap are the operative bounds on
runaway CPU and memory use, not a `setrlimit` on either directly.

**Degraded mode.** On macOS, or on a Linux host where unprivileged user
namespaces are unavailable, none of the above isolation is possible; steps
run directly against the real scratch tree at its real physical path
instead of `/mnt/...`, with full host network access and no masking. The
dashboard and the daemon log make this visibility gap explicit rather than
silently pretending the isolation happened; see
[Troubleshooting](../operations/06-ci-yml-reference.md#troubleshooting).

## Version-stamping recipe

The sandbox checkout comes from `git archive`, so it has no `.git`
directory and no way to run `git describe` inside a step. Two things work
around that, and this project's own `.ckgit/ci.yml` uses both:

- **`CKGIT_COMMIT`** ([§6](#environment)) is the exact commit id, supplied
  by the runner directly — no git command needed inside the sandbox at all.
- **`.ckgit/build-commit`**, a one-line file containing the `git archive`
  `export-subst` placeholder `$Format:%H$`, with the matching
  `.gitattributes` line `/.ckgit/build-commit export-subst`. Because the
  runner materializes every checkout with `git archive`, that placeholder is
  substituted with the real archived commit hash in the tree a step actually
  sees — a fallback that also works for an older runner build that predates
  the `CKGIT_COMMIT` export, or for a source tarball built entirely by hand
  outside any CI system.

This project's own Makefile (`CKGIT_BUILD_VERSION`) is the worked example: it
prefers a real `.git` checkout's `git describe`, falls back to
`.ckgit/build-commit` when there is no `.git` at all, and falls back again to
a plain `+source` suffix if neither is available; an exact
`CKGIT_BUILD_VERSION=` passed on the command line always wins outright. This
project's own `.ckgit/ci.yml` `build` step implements the same fallback
chain explicitly in shell (preferring `CKGIT_COMMIT`, then
`.ckgit/build-commit`), so it works even against an older runner, and stamps
a release build's version with its tag name too when `CKGIT_REF` names one.
