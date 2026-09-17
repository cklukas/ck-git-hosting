# Continuous integration

This guide turns on per-project CI, writes a first `.ckgit/ci.yml`, and shows
where results appear and how the runner is operated. The design and security
rationale are in [../planning/09-ci-cd.md](../planning/09-ci-cd.md).

CI is off for every project until an administrator enables it. When it is on, a
push to a triggering branch runs that commit's `.ckgit/ci.yml` on the server in
an isolated sandbox, and the result appears read-only in the dashboard. It is
built for a trusted LAN whose project members already have push access; it is
not a shared, multi-tenant build farm.

## What the package installs

The server package and `packaging/install.sh` add the runner alongside the
daemon:

| Path | Owner and mode | Purpose |
|---|---|---|
| `/usr/bin/ck-ci-runnerd` | `root:root 0755` | the CI runner binary |
| `/usr/lib/systemd/system/ck-ci-runner.service` (package) or `/etc/systemd/system/ck-ci-runner.service` (script) | `root:root 0644` | hardened runner unit |
| `/var/lib/ck-git-hosting/ci-build` | `ckgit:ckgit 0700` | per-run scratch build root |

Run records and logs live under the private state root
(`/var/lib/ck-git-hosting/state/ci`), which the runner creates on first use.

The runner unit runs as `ckgit`, the same account as the daemon, so it can read
the job spool the receive hook writes. Unlike the daemon it is allowed to create
the unprivileged user, mount, and network namespaces that confine each build
step; that is why its unit carries `RestrictNamespaces=user mnt net` and, unlike
the daemon, sets no system-call allow-list (a build runs arbitrary tools whose
syscall surface is open-ended).

## Configure the runner

The runner reads the same `/etc/ck-git-hosting/server.ini` as the daemon. The
daemon ignores the CI keys; only the runner uses them. `ci_build_root` is
required for the service; the rest are optional with the defaults shown.

```text
ci_build_root=/var/lib/ck-git-hosting/ci-build
# ci_timeout_seconds=1800            # per-step wall-clock budget (1..86400)
# ci_max_log_bytes=1048576           # per-step captured-output cap (1024..1073741824)
# ci_poll_seconds=5                  # spool poll interval (1..3600)
# ci_allow_network=false             # true gives steps a network namespace with interfaces
# ci_cache_root=/var/lib/ck-git-hosting/ci-cache   # persist `cache:` dirs (ccache, ...) across runs; unset = per-run
# --- artifact retention (enforced by the runner's periodic sweep) ---
# ci_artifact_retention_days=7       # default lifetime of an ephemeral artifact (1..3650)
# ci_artifact_max_retention_days=90  # cap on a workflow's own retention_days
# ci_artifact_max_bytes=268435456    # per-artifact bundle cap (>=1024); larger is dropped
# ci_artifact_max_project_bytes=2147483648   # per-project budget; 0 disables (evict oldest)
# ci_artifact_max_total_bytes=10737418240     # global budget; 0 disables (evict oldest)
# ci_artifact_keep_latest=true       # keep each project's newest run's artifacts
# ci_runs_keep=200                   # keep this many run directories per project (0 = all)
# ci_cleanup_interval_seconds=3600   # how often the sweep runs (60..86400)
# --- pages hosting (served by ck-pagesd, a separate origin; see Pages below) ---
# pages_root=/var/lib/ck-git-hosting/pages   # where published sites are stored (set by a fresh install)
# pages_http_port=8421               # ck-pagesd's port (LAN-exposable); then enable ck-pages.service
# pages_keep_versions=3              # site versions kept for rollback (1..1000)
```

A fresh install written with `packaging/install.sh` already sets
`ci_build_root`. Confirm the daemon still parses the file after any edit
(`--check` prints only the daemon's own keys, which is expected — it does not
echo the CI keys):

```text
sudo ck-git-hostingd --config /etc/ck-git-hosting/server.ini --check
```

Restart the runner after changing any CI key:

```text
sudo systemctl restart ck-ci-runner.service
```

## Enable CI for a project

Opt in per project with `ckgit-admin`. Point it at the configuration file so it
finds the state root:

```text
sudo ckgit-admin ci enable myproject --config /etc/ck-git-hosting/server.ini
sudo ckgit-admin ci status myproject --config /etc/ck-git-hosting/server.ini
```

`ci status` prints `myproject: CI enabled` or `myproject: CI disabled`. Use
`ci disable` to turn it off again; disabling stops new runs but keeps existing
run history. Removing the project also clears its CI opt-in and records.

## Write `.ckgit/ci.yml`

Commit the workflow at `.ckgit/ci.yml` in the repository. It is read from the
pushed commit, so a build always matches the commit that triggered it.

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
  projects are allowed. Write `- name` for the default branch, or
  `- { name: lib, ref: v1.2.0 }` to pin. See below.
- `cache:` lists persistent build caches (e.g. a ccache store) kept across runs.
  Each is a directory exported as `CKGIT_CACHE_<NAME>`, plus any variables bound
  with `env:`. Persistence needs `ci_cache_root` set on the server; without it a
  declared cache still works, only per-run, so a workflow is portable. See below.

The format is a strict, bounded subset of YAML — not GitHub Actions. Unknown
keys, tabs for indentation, wrong types, or anything past the documented size
limits are rejected, and the run is recorded `error` with the reason. Keep a
workflow well under 64 KiB, 64 jobs, and 128 steps per job.

## Sister projects and build caches

A suite whose parts live in separate repositories can build them together with
no network access, and keep repeat builds fast, by declaring the dependencies
as `sisters` and a compiler cache as a `cache`:

```text
version: 1
sisters:
  - ckmath
  - { name: ckvision, ref: v0.5.0 }
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
[below](#the-sandbox-and-its-requirements)), the checkout and this project's
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

Durable release assets, attached to a tag, never expire; see Releases below.

## Push and read results

Push to a triggering branch or tag as usual. The receive hook queues one job per
updated branch head or tag; the runner picks it up within `ci_poll_seconds`.

In the dashboard, open a project and follow the **CI** tab:

- `/<...>/project/<id>/ci` lists runs with an at-a-glance status icon, branch,
  commit, and timing — how long a finished run took, or how long a running one
  has been going. The list refreshes itself while a run is in progress.
- `/<...>/project/<id>/ci/<run-id>` is one run's live status page: overall
  status with elapsed time, each step's result, the running step's output tail,
  and artifacts. While the run is active the page auto-refreshes (no JavaScript —
  a plain `<meta refresh>` that fits the dashboard's strict content policy) and
  stops once the run is terminal.
- `/<...>/project/<id>/ci/<run-id>/<step>.log` shows one step's full output, with
  a **Follow live** button that tails the running step in real time over
  Server-Sent Events (`<step>.stream`). This is the one page that runs a small,
  nonce-scoped script under a relaxed policy; the static log still works without
  JavaScript, and the number of concurrent live streams is capped so followers
  cannot starve the dashboard.

The projects index also carries a **Last CI** column with each project's newest
run status and timing, so a running or failed build is visible without opening
the project.

Run status is one of `success`, `failure` (a step exited non-zero), `timeout` (a
step exceeded its budget), `error` (the run could not be set up — for example a
malformed workflow), `cancelled` (stopped on request — see below), or `skipped`
(no workflow, or a non-triggering branch). A `running` build whose runner stops
reporting is shown as **interrupted** until the next runner sweep settles it.

### Watch and cancel a run

Progress and cancellation are available three ways, all reading the same run
records and, for a stop, dropping one cooperative cancel marker that the runner
honours between and within steps — it kills the current step's process group and
records the run `cancelled`, well before a long step would finish on its own:

- **Dashboard** — the live run page has a **Cancel run** button. It is the read-
  only dashboard's one mutating action: a loopback-only, same-origin `POST` to
  `/project/<id>/ci/<run-id>/cancel`.
- **CLI** — `ckgit-admin`, on the server, reads and cancels directly:

  ```text
  sudo ckgit-admin ci runs   myproject --config /etc/ck-git-hosting/server.ini
  sudo ckgit-admin ci log    myproject <run-id> --follow --config /etc/ck-git-hosting/server.ini
  sudo ckgit-admin ci cancel myproject <run-id> --config /etc/ck-git-hosting/server.ini
  ```

  `ci runs` lists recent runs with status and timing; `ci log` prints a step's
  output and, with `--follow`, streams the running step live; `ci cancel`
  requests cancellation.
- **Control socket** — the daemon's same-user socket answers `ci-status
  <project>` (the newest runs as `run_id status started heartbeat finished steps`
  lines) and `cancel <project> <run-id>`. The dashboard's button drives this
  path; it is also scriptable locally.

On the command line, the records are also plain files under the state root:

```text
sudo ls /var/lib/ck-git-hosting/state/ci/runs/myproject
sudo cat /var/lib/ck-git-hosting/state/ci/runs/myproject/<run-id>/run.ini
```

## Releases

Pushing a **tag** turns a build into a durable release — this is where v1
installers live. When a tag build's job declares `artifacts:`, those bundles are
stored under the tag and kept until the tag is deleted; they never expire and the
retention sweep never touches them. A tag triggers a release when it matches
`on: { tags: [...] }`, or always when the workflow omits `on:`.

- The **Releases** tab lists each tag newest-first, using the annotated tag's
  message as the release notes, with every asset's size, checksum, and a
  download link.
- A failed tag build publishes nothing.
- Deleting the tag removes its release and every asset; deleting the project
  removes them too. There is no expiry and no admin step — pushing and deleting
  tags is the whole workflow.

```text
git tag -a v1.0.0 -m 'Release 1.0'      # annotate for release notes
git push origin v1.0.0                   # build and publish the release
git push origin :refs/tags/v1.0.0        # delete the tag -> delete the release
```

Download a release asset the same way as a CI artifact:

```text
curl -O http://<server>:<http_port>/project/myproject/releases/<tag>/<name>
```

## Pages

A build can publish a static site that anyone on the LAN can browse. Add a
top-level `pages:` block naming the directory to publish:

```text
version: 1
pages: { path: public }
jobs:
  - name: site
    steps:
      - run: make site        # writes ./public
```

The site is published from a **successful build of the repository's default
branch** — feature-branch and tag builds never replace it. Each publish is a new
versioned copy, and the newest `pages_keep_versions` are kept, so a bad deploy
can be rolled back by re-running an earlier commit's build. Deleting the project
removes its sites.

If the steps pass but publishing the site fails — for example the site exceeds
`kMaximumPagesSiteBytes` — the **run is marked failed** (with the reason in its
detail) rather than reporting success while the live site silently stays on the
previous version. Keep generated caches (a Sphinx `.doctrees` directory, say)
out of the published directory so they do not count against the size limit.

### Serving it on the intranet

Sites are served by a **separate process, `ck-pagesd`, on its own port** — a
different origin from the dashboard on purpose, because a site's own JavaScript
must never run on the dashboard's origin (that is exactly why GitHub uses
`github.io` and GitLab `*.gitlab.io`). Turn serving on:

1. Set `pages_root` and `pages_http_port` in `server.ini` (a fresh install
   already sets `pages_root`; just uncomment `pages_http_port`).
2. `sudo systemctl enable --now ck-pages.service`.

Then browse `http://<server>:<pages_http_port>/<project>/`. For a friendly name
with no DNS server, enable mDNS on the host (`sudo apt install avahi-daemon`)
and use `http://<host>.local:<pages_http_port>/<project>/` — that works on
macOS, Linux, and Windows 10+ (Android browsers are unreliable over mDNS; use
the IP there). True per-project subdomains (`http://<project>.pages.<host>/`)
need a LAN resolver with a wildcard entry, such as `dnsmasq`, and are a later
option — the port form needs no DNS at all.

`ck-pagesd` serves read-only static files only, resolves each path without
following symlinks, and runs under a tight sandbox with no write access and no
access to the control socket, the repositories, or the metadata store. Firewall
`pages_http_port` to the intended subnet.

**Trust note:** a site's JavaScript is as trusted as whoever can push to the
project — it runs in the visitor's browser, the same property GitHub Pages has.
Serving it on its own origin protects the dashboard; it does not vet the site's
content. On a trusted-committer LAN this is the accepted trade.

## The sandbox and its requirements

Each step runs with a wall-clock timeout, an output-size cap, a scrubbed
environment, and no core dumps or single file over 4 GiB (`RLIMIT_CORE` and
`RLIMIT_FSIZE`; the timeout and output cap are the operative bounds on CPU and
memory use, not a `setrlimit` on either). On Linux it is additionally placed in
its own user, mount, and network namespace: it has no network beyond loopback
unless `ci_allow_network=true`, and cannot see the host's mount table.

On Linux the mount namespace also **masks the service tree**: the private
state root, the CI build root (other runs' scratch), the cache root (other
projects' caches), Pages, and the bare-repository root are each covered with
an empty, private `tmpfs`, so a step can see and write only its own checkout
and its own declared caches — not the CI spool or run records, not another
project's release or cache, not the repositories directly (a `sisters` entry
is the sanctioned, read-only way to reach another hosted project's source).

The network and mount isolation, and the masking above, need **unprivileged
user namespaces** enabled on the host. On Debian they are on by default;
confirm with:

```text
sysctl kernel.unprivileged_userns_clone   # 1 means enabled (if the key exists)
cat /proc/sys/user/max_user_namespaces    # a non-zero value
```

If they are disabled, the runner still enforces the timeout, output cap, and
rlimits, and the working directory is still the scratch checkout, but there is
no network or mount isolation at all: a step then sees the whole host
filesystem exactly as the runner account does, and `ck-ci-runnerd` logs a
warning to that effect. When namespaces are available but the mount masking
itself could not be established (a restrictive LSM, say), a separate warning
names that specifically, since the namespace warning above does not fire in
that case. Enable user namespaces, or do not enable CI for untrusted work, if
that isolation matters to you. macOS builds of the runner always operate in
this degraded mode; run production CI on Linux.

## Operate the service

```text
systemctl status ck-ci-runner.service
journalctl -u ck-ci-runner.service          # runner log, including degraded-mode warnings
sudo systemctl restart ck-ci-runner.service # after a server.ini change
```

To reproduce one run by hand without the spool:

```text
sudo -u ckgit ck-ci-runnerd run \
  --repo /srv/ck-git-hosting/repos/myproject.git \
  --commit <sha> --project myproject \
  --state-root /var/lib/ck-git-hosting/state \
  --build-root /var/lib/ck-git-hosting/ci-build
```

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| No run appears after a push | CI not enabled for the project (`ckgit-admin ci status`), or the runner is not running (`systemctl status ck-ci-runner.service`). |
| Every run is `skipped` | The branch is not a trigger; add it to `on: { branches: [...] }`, or push the default branch. A commit with no `.ckgit/ci.yml` is also skipped. |
| Run is `error` before any step | The workflow is malformed or over a size limit; the `run.ini` detail names the reason. |
| A step cannot reach the network | Expected: the sandbox denies the network by default. Set `ci_allow_network=true` and restart the runner if a build genuinely needs it. |
| Log warns about missing isolation | Unprivileged user namespaces are disabled or unavailable (see above). |
| `ci` admin command errors about state | Pass `--config /etc/ck-git-hosting/server.ini` (or `--state-root`) so it can find the state root, and run it as root or `ckgit`. |
