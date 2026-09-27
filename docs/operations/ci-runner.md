# Configure and operate the CI runner

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
# ci_allow_network=false             # true skips network isolation: the step shares the host's real network
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
# --- pages hosting (served by ck-pagesd, a separate origin; see the Pages hosting guide) ---
# pages_root=/var/lib/ck-git-hosting/pages   # where published sites are stored (set by a fresh install)
# pages_http_port=8421               # ck-pagesd's port (LAN-exposable); then enable ck-pages.service
# pages_keep_versions=3              # site versions kept for rollback (1..1000)
# pages_public_url=https://pages.example.lan   # exact base URL for site links; overrides the derived one
```

The dashboard normally derives a project's Pages site link from the
advertised SSH clone host plus `pages_http_port`, but a dashboard request
often arrives through a loopback tunnel that cannot reach that port itself.
Set `pages_public_url` to an `http(s)://` URL (no path) when the derived link
would be wrong -- a reverse proxy, a different public hostname, or a
different port than `pages_http_port` -- and every project's site link uses
it verbatim instead.

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

### Sizing on a Raspberry Pi

A Raspberry Pi 4 is a realistic target for this daemon, but its four Cortex-A72
cores are slow for C++ compilation. Measured on real Pi 4 hardware, `make
all` for this project's own C++20 sources -- six binaries, `-O2 -Wall -Wextra
-Wpedantic -Werror`, no parallel `-j` -- takes just under 7 minutes wall
clock. A `check` step that also builds and runs the unit binary plus every
`tests/integration/*.sh` script (several of which wait on real multi-second
timeouts, independent of CPU speed) adds meaningfully more on top of that. The
default `ci_timeout_seconds=1800` (30 minutes) has headroom above the plain
build alone, but a `.ckgit/ci.yml` whose `check` step mirrors this project's
own -- `make ... all check` in one step -- should raise `ci_timeout_seconds`
well past the default rather than risk a slow but otherwise successful build
being killed at the wall-clock limit; there is no way to distinguish "still
building" from "hung" from outside the step. This project's own Pi 4 server
runs with `ci_timeout_seconds=14400` (4 hours) for exactly this reason -- a
generous, rarely-hit ceiling rather than a tight one tuned to the common case.
Raise `ci_max_log_bytes` too (the default 1 MiB) if the full test suite's
combined output would otherwise be truncated; this server runs with
`ci_max_log_bytes=16777216` (16 MiB).

## The sandbox and its requirements

Each step runs with a wall-clock timeout, an output-size cap, a scrubbed
environment, and no core dumps or single file over 4 GiB (`RLIMIT_CORE` and
`RLIMIT_FSIZE`; the timeout and output cap are the operative bounds on CPU and
memory use, not a `setrlimit` on either). On Linux it is additionally placed in
its own user and mount namespace, and, unless `ci_allow_network=true`, its own
network namespace too: it has no network beyond loopback, and cannot see the
host's mount table. `ci_allow_network=true` skips the network namespace
entirely rather than granting the step an isolated one of its own, so a step
run that way sees the host's real interfaces and the LAN, not a sandboxed copy.

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
