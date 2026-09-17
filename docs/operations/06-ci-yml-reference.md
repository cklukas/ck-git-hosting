# `.ckgit/ci.yml` reference

This is the complete reference for the workflow file self-hosted CI reads:
every accepted and rejected syntax construct, every key, every bound, the
exact sandbox and environment a step runs in, and how a build becomes an
artifact, a release, or a published Pages site. [Continuous
integration](04-ci-cd.md) is the tutorial that gets a first workflow running;
come here for what it does not spell out.

## 1. File location and reading rules

The workflow is read from the **pushed commit object** —
`git cat-file -p <commit>:.ckgit/ci.yml` — never from a working tree and
never from any other commit. A build always matches the exact commit that
triggered it, and a workflow edited on a branch takes effect starting with
the commit that changed it, not retroactively.

A commit that has no `.ckgit/ci.yml` at all records the run as **skipped**,
with no error. This is normal for a project that has not opted into CI, and
for a commit that predates the file being added.

## 2. Syntax

The format is a small, strict, hand-rolled subset of YAML — not a full YAML
parser, and not GitHub Actions: there is no `uses:`, no marketplace, no
`${{ }}` expression language, and no implicit type coercion. Anything the
parser does not explicitly understand is rejected, not guessed at.

**Accepted:**

- Block mappings (`key:` / indented `key: value` lines) and block sequences
  (`- item`), and flow mappings `{a: b, c: d}` and flow sequences `[a, b]`,
  nested up to 16 levels deep.
- Plain scalars (unquoted text), single-quoted scalars (`'...'`, with `''`
  as the only escape, meaning a literal `'`), and double-quoted scalars
  (`"..."`).
- Exactly four backslash escapes inside a double-quoted scalar: `\\`, `\"`,
  `\n`, `\t`. Any other escape (`\r`, `\u...`, `\/`, ...) is rejected.
- `|` literal block scalars for multi-line `script:` bodies. The body is
  every following line indented deeper than the `script:` key; the first
  non-blank such line sets the base indent, and a later line indented less
  than that is rejected.
- `#` comments: a `#` starts a comment only outside quotes and outside flow
  brackets, and only at the start of a line or immediately after a space —
  `value#not-a-comment` keeps the `#`, `value # comment` strips it.
- Spaces-only indentation, LF-only line endings, UTF-8-only content.

**Rejected, with the exact reason:**

- **Tabs for indentation** — `"tabs may not be used for indentation"`.
- **Carriage returns** — `"carriage returns are not allowed (use LF line
  endings)"`. A file with CRLF endings must be converted first.
- **`>` folded block style, and `|-`/`|+`/`>-`/`>+` chomping indicators** —
  none of these are recognized as block-style syntax at all; only the exact
  token `|` triggers block-literal parsing. `script: >` or `script: |-`
  parse as an ordinary one-or-two-character plain scalar (the literal text
  `>` or `|-`) instead, which then almost always fails downstream as
  nonsensical shell.
- **Duplicate keys** in the same mapping, and **unknown keys** anywhere the
  schema defines a fixed set (see the schema table below) — both rejected
  outright, never silently overwritten or ignored.
- **Anchors, aliases, tags, `---`/`...` document markers, and `<<:` merge
  keys** are never recognized as such — there is no anchor/alias resolution
  in this parser at all. In practice this means: a merge key is rejected by
  the key-character rule (`<` is not allowed in a key); a document marker on
  its own line has no `:` and fails as an invalid entry; `key: &anchor
  value` or `key: !!str value` are parsed as the literal plain-scalar text
  `&anchor value` / `!!str value`, which is then accepted or rejected purely
  by whatever that field's own validation does with an ordinary string (a
  job or artifact name rejects it on charset grounds; a plain `env:` value,
  which has no charset restriction, would actually accept it as that literal
  string — this is a real difference from "rejected", worth knowing if you
  ever see one of these bytes survive into a build unexpectedly).
- A **flow sequence's items are always scalars**, never nested mappings or
  sequences — `[a, {b: c}]` does not give you a list containing a mapping;
  the text `{b: c}` (braces included) becomes one literal scalar string. A
  flow *mapping's* values, by contrast, can be nested (`{a: [1, 2]}`,
  `{a: {b: c}}`), up to the same 16-level depth limit.
- **A block sequence item that starts with a flow mapping on the same
  line**, for example `- { name: lib, ref: v1.2.0 }`, does **not** parse the
  way it looks like it should: the parser finds the first colon-followed-by-
  space to decide whether the item is itself a small mapping, and that scan
  does not track bracket depth, so it locks onto the colon *inside* the
  braces (after `name`) rather than treating the whole `{ ... }` as one
  value. The result is a rejected key `{ name` (the literal `{` fails the
  key-character rule). **Always use the two-line block form** for a `sisters:`
  or `cache:` entry that needs more than a bare name — see the schema table
  and worked examples below. (`pages: { path: public }` and similar *are*
  fine: there the real key comes before the brace, so the scan finds the
  genuine key/value colon correctly. The failure is specific to a flow
  mapping opening immediately after a sequence dash.)
- Anything over the size and count bounds in [§10](#10-bounds-table) below.

## 3. Schema table

| Key | Type | Required | Default | Validation |
|---|---|---|---|---|
| `version` | integer | yes | — | must be exactly `1` |
| `on.branches` | list of strings | no | none | valid branch name each; see [trigger matrix](#4-trigger-matrix) |
| `on.tags` | list of strings | no | none | a tag pattern: an exact name, or a name ending in a single trailing `*` wildcard |
| `env` (top level) | mapping | no | empty | keys: `[A-Za-z_][A-Za-z0-9_]*`; values: any scalar |
| `sisters[]` | list of a bare name or a `{name, ref}` mapping | no | empty | `name`: a valid project name (same rule as a hosted project's own name); `ref` (optional): a branch, tag, or commit-like ref, not starting with `-`/`/`, no `..` |
| `cache[]` | list of a bare name or a `{name, env}` mapping | no | empty | `name`: a valid CI name (letters, digits, `. _ -`); `env` (optional): list of variable names to also bind to the cache path |
| `pages.path` | string | required *within* `pages:` | — | a safe relative path: no leading `/`, no `.`/`..` component, no backslash |
| `jobs[]` | list of mappings | required, non-empty | — | unique `name` per job |
| `jobs[].name` | string | required | — | valid CI name |
| `jobs[].env` | mapping | no | empty | same rule as top-level `env`; merged **over** it at run time |
| `jobs[].steps` | list of mappings | required, non-empty | — | — |
| `jobs[].artifacts` | mapping | no | none | see below |
| `steps[].name` | string | no | falls back to the job's name | no charset restriction, just a length cap |
| `steps[].run` (list form) | list of strings | exactly one of `run`/`script` required | — | executed as a literal argument vector, **no shell** |
| `steps[].run` (scalar form) | string | (same) | — | executed as `sh -ec '<value>'` |
| `steps[].script` | string, typically a `\|` block | (same) | — | executed as `sh -ec '<value>'` |
| `artifacts.name` | string | no | the owning job's name | valid CI name; **must not be `release`** — that name is reserved for the tag build's own release record |
| `artifacts.paths` | list of strings | required, non-empty | — | each a safe relative path under the checkout |
| `artifacts.retention_days` | integer | no | `0` (use the server default) | `0` is rejected if written explicitly; capped to the server's configured maximum; **ignored entirely on a tag/release build**, which is always durable regardless of this value |

**Reserved names**, enforced at different times:

- An artifact named `release` is rejected while **parsing** the workflow
  (`interpretArtifact`), whether written explicitly or defaulted from a job
  named `release`.
- A sister named `src`, `home`, or `tmp` — the sandbox's own fixed mount
  points — parses successfully (these are ordinary valid names) and is only
  rejected when the runner actually **executes** the workflow. A syntax-only
  check of a workflow cannot catch this; only a real run can. The same is
  true of a sister that names the project it belongs to.

## 4. Trigger matrix

| `on:` in the file | Branch push | Tag push |
|---|---|---|
| absent entirely | only the repository's **default branch** triggers | **every** tag triggers a release |
| `on: { branches: [...] }` (no `tags:`) | only the listed branches trigger | **no tag ever triggers**, including one matching a branch name |
| `on: { tags: [...] }` (no `branches:`) | **no branch ever triggers, not even the default branch** | only tags matching a listed pattern trigger |
| `on: { branches: [...], tags: [...] }` | only the listed branches | only tags matching a listed pattern |

A branch match is an exact string comparison against the pushed branch name
— there is no wildcard support for branches. A tag pattern is either an
exact name or, if it ends in `*`, a prefix match (an empty prefix, the bare
pattern `*`, matches any tag).

Branch **deletions** queue no CI job at all — no run record of any kind.
Tag **deletions** also queue no job, but do delete that tag's release record
and every one of its assets, if it has one.

One CI job is queued **per updated ref in a push**, unconditionally — the
receive hook never parses the workflow or evaluates `on:` itself; that
happens only once the runner claims the job. This is why a push to a
non-triggering branch still produces a visible run in the dashboard: the job
really was queued, and the run really was created — it simply resolved to
**skipped** once the runner read the workflow and matched the ref.

A tag additionally has to be a syntactically valid release tag (letters,
digits, `. _ -` only, no `/`, at most 128 bytes) to trigger a release, even
if it matches an `on.tags` pattern; an invalid tag's run is skipped with a
stated reason.

Jobs are claimed from the spool **oldest first**.

## 5. Execution

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

## 6. Environment

Every step runs with exactly these variables, before anything the workflow
itself adds:

| Variable | Value |
|---|---|
| `PATH` | `/usr/local/bin:/usr/bin:/bin` |
| `HOME` | the sandbox home directory (`/mnt/home`; see [§7](#7-filesystem)) |
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

## 7. Filesystem

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
[Troubleshooting](#12-troubleshooting).

## 8. Version-stamping recipe

The sandbox checkout comes from `git archive`, so it has no `.git`
directory and no way to run `git describe` inside a step. Two things work
around that, and this project's own `.ckgit/ci.yml` uses both:

- **`CKGIT_COMMIT`** ([§6](#6-environment)) is the exact commit id, supplied
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

## 9. Artifacts, releases, and Pages

**Packing.** An `artifacts:` block is packed only after **every** step in
its job has succeeded — a job with any failed step never has its artifact
packed, not even partially. Packing runs `tar` over the declared `paths`,
relative to the checkout, outside the sandbox (it is the runner's own
scratch tree being packed, not untrusted step execution), capped at the
server's configured artifact-bundle byte limit. A bundle over the cap, or a
`tar` failure (for example a declared path that does not exist), is
recorded as a failed artifact and nothing is stored — the run itself is
unaffected by an artifact failure, since it already succeeded.

**Branch build vs. tag/release build** — the same `artifacts:` block
behaves differently depending on what triggered the run:

| | Branch build (ephemeral) | Tag build (release, durable) |
|---|---|---|
| Where it lands | the run's own artifact store, alongside its logs | the project's durable release store, under the pushed tag |
| Expiry | `retention_days` (the workflow's value, or the server default; always capped to the server's maximum) | never — a release asset has no expiry at all |
| Removed by | the periodic retention sweep (age, per-project or global byte budget, or run-count pruning) | only an explicit tag deletion, or removing the project |
| A failed run | nothing is packed | **the entire release for that tag is removed** — a failed tag build publishes nothing, even if an earlier attempt at the same tag once succeeded |

A tag build additionally writes a release record (the tag, its commit, when
it was created, and the annotated tag's message as release notes — empty
for a lightweight tag) alongside the asset. Both a normal CI run record
(with its logs) *and* the separate durable release record exist for a
successful tag push; they are not the same thing and are not stored in the
same place.

**Pages.** A `pages: { path: ... }` block publishes only when **all** of
these hold for a given run: every job's every step succeeded; the build was
**not** a tag/release build (a tag never publishes Pages, regardless of
which branch it points at); the server has Pages storage configured at all;
this push's branch is exactly the repository's own default branch; and the
declared `path`, relative to the checkout, exists as a directory once the
steps finish. If the path does not exist, or the default branch cannot be
resolved, publishing is silently skipped — not an error, the run still
reports whatever the steps themselves produced. If the site directory
*does* exist and publishing itself fails (for example the site is over the
Pages size or file-count cap), the run's outcome is overwritten to `error`
even though every step passed — a build that silently kept the previous
live site is worse than a failed run, so that failure is never hidden.

## 10. Bounds table

Every one of these throws before a step ever runs — either during parsing
(a `std::length_error` naming which bound, or `std::runtime_error` for any
other malformed input) or, for the run-time-only entries at the bottom, once
the runner acts on the parsed workflow.

| Bound | Value | Applies to |
|---|---|---|
| Whole file | 65536 bytes (64 KiB) | the complete `.ckgit/ci.yml` |
| Per physical line | 4096 bytes | any one line, before indentation/CR checks |
| Total physical lines | 4096 | the whole file |
| Jobs | 64 | `jobs:` |
| Steps per job | 128 | `jobs[].steps:` |
| `env:` entries | 64 | top-level or per-job `env:`, checked separately |
| Branches / tag patterns | 64 each | `on.branches` and `on.tags` share this one bound |
| Items in one `run:` list | 64 | a step's `run:` as a list |
| Name length | 64 bytes | job, cache, and artifact names |
| Key length | 128 bytes | any mapping key |
| Plain/quoted scalar | 4096 bytes | any scalar value, including a `run:` scalar |
| `script:` block | 16384 bytes (16 KiB) | a `\|` block or `script:` value |
| Artifact `paths:` entries | 64 | one artifact block |
| Path length | 1024 bytes | any `paths:`/`pages.path` entry |
| Sisters | 16 | `sisters:` |
| Caches | 8 | `cache:` |
| `env:` names per cache | 8 | `cache[].env:` |
| `retention_days` | 3650 (10 years) | `artifacts.retention_days`, as an upper cap |
| Flow/block nesting | 16 levels | any nested mapping or sequence |

Two further, server-configured bounds are not part of the workflow schema
itself but shape what a step and its artifact can do in practice: the
per-step log cap (`ci_max_log_bytes`, default 1 MiB) and the per-artifact
bundle cap (`ci_artifact_max_bytes`, default 256 MiB) — see
[Configure the runner](04-ci-cd.md#configure-the-runner) for the full,
current key table. A published Pages site has its own separate caps, entirely
unrelated to the workflow file: 8 GiB total, 1 GiB per file, and 100000
entries.

## 11. Worked examples

**A plain `make` project:**

```yaml
version: 1
on: { branches: [main], tags: [v*] }
jobs:
  - name: build-and-test
    steps:
      - run: [make, all]
      - name: tests
        run: make check
    artifacts:
      name: build
      paths: [dist]
```

**CMake, ccache, and one pinned sister:**

```yaml
version: 1
sisters:
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
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
          cmake --build build
      - name: tests
        run: [ctest, --test-dir, build, --output-on-failure]
```

**A docs site published from the default branch, with releases on tags:**

```yaml
version: 1
on: { branches: [main], tags: [v*] }
jobs:
  - name: docs
    steps:
      - run: [make, site]
    artifacts:
      name: packages
      paths: [dist]
pages:
  path: site/public
```

Every push to `main` builds and, on success, publishes `site/public` as the
project's Pages site. A tag push instead produces a durable release from the
same `packages` artifact, and does **not** touch Pages at all — the two
outcomes are mutually exclusive per run, driven entirely by whether the
triggering ref was a tag.

**This project's own workflow, annotated** — the real, in-repository
`.ckgit/ci.yml` this project builds, tests, and releases itself with:

```yaml
version: 1
on:
  branches: [master]
  tags: [v*]
jobs:
  - name: build-test-package
    steps:
      - name: build
        script: |
          # computes a version string from CKGIT_COMMIT or .ckgit/build-commit
          # (see §8), then: make BUILD_ROOT=... CKGIT_BUILD_VERSION=... all
      - name: check
        script: |
          # make ... check -- the unit binary plus every tests/integration/*.sh
      - name: package
        script: |
          # refuses a tag that does not match VERSION, builds .debs and a
          # source tarball with packaging/build-deb.sh, writes build-version
    artifacts:
      # never "release": that name is reserved for the tag build's own
      # release record
      name: packages
      paths: [dist]
```

Read the full file in the repository root for the exact shell; the shape
above is what matters for a reference — one job, three steps that share
state through `$TMPDIR` (set by the runner, not the workflow), and one
artifact whose name was deliberately chosen to avoid the reserved `release`.

## 12. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| No run appears after a push | CI is not enabled for the project (`ckgit-admin ci status`), or the runner is not running (`systemctl status ck-ci-runner.service`). |
| Every run is `skipped` | The branch is not a trigger under [§4](#4-trigger-matrix); add it to `on: { branches: [...] }`, or push the default branch. A commit with no `.ckgit/ci.yml` is also skipped. |
| Run is `error` before any step | The workflow is malformed, or over one of the bounds in [§10](#10-bounds-table); the run's own detail names the reason. |
| A step cannot reach the network | Expected: the sandbox denies the network beyond loopback by default. Set `ci_allow_network=true` and restart the runner if a build genuinely needs it — see the networking note in [§7](#7-filesystem) for exactly what that changes. |
| A `sisters:`/`cache:` entry with extra fields is rejected as an unsupported key | The single-line `- { name: ..., ref: ... }` form does not parse — see the syntax note in [§2](#2-syntax); use the two-line block form instead. |
| Log warns about missing isolation, or that filesystem masking could not be confirmed | Unprivileged user namespaces are disabled or unavailable on this host — see degraded mode in [§7](#7-filesystem). A step still ran, but without network or filesystem isolation from the rest of the server. |
| `ckgit-admin ci ...` errors about state | Pass `--config /etc/ck-git-hosting/server.ini` (or `--state-root`) so it can find the state root, and run it as root or the `ckgit` account. |
