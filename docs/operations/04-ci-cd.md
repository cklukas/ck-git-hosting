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
# ci_timeout_seconds=1800     # per-step wall-clock budget (1..86400)
# ci_max_log_bytes=1048576    # per-step captured-output cap (1024..1073741824)
# ci_poll_seconds=5           # spool poll interval (1..3600)
# ci_allow_network=false      # true gives steps a network namespace with interfaces
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
```

- `version:` must be `1`.
- `on: { branches: [...] }` limits which branches trigger the workflow. Omit
  `on:` to trigger only on the repository's default branch. A push to any other
  branch is recorded `skipped`, as is a commit with no `.ckgit/ci.yml`.
- `env:` is fixed `key: value` pairs; there is no `$VAR` expansion. A job's
  `env:` is merged over the top-level `env:`.
- A step's `run:` written as a **list** is an exact command with no shell. A
  `run:` **scalar** or a `script:` block runs with `sh -ec` inside the sandbox.

The format is a strict, bounded subset of YAML — not GitHub Actions. Unknown
keys, tabs for indentation, wrong types, or anything past the documented size
limits are rejected, and the run is recorded `error` with the reason. Keep a
workflow well under 64 KiB, 64 jobs, and 128 steps per job.

## Push and read results

Push to a triggering branch as usual. The receive hook queues one job per
updated branch head; the runner picks it up within `ci_poll_seconds`.

In the dashboard, open a project and follow the **CI** tab:

- `/<...>/project/<id>/ci` lists runs with status, branch, commit, and timing,
  and links each step to its log.
- `/<...>/project/<id>/ci/<run-id>/<step>.log` shows one step's captured output.

Run status is one of `success`, `failure` (a step exited non-zero), `timeout` (a
step exceeded its budget), `error` (the run could not be set up — for example a
malformed workflow), or `skipped` (no workflow, or a non-triggering branch).

On the command line, the records are plain files under the state root:

```text
sudo ls /var/lib/ck-git-hosting/state/ci/runs/myproject
sudo cat /var/lib/ck-git-hosting/state/ci/runs/myproject/<run-id>/run.ini
```

## The sandbox and its requirements

Each step runs with a wall-clock timeout, an output-size cap, resource limits, a
scrubbed environment, and a scratch-only working directory. On Linux it is
additionally placed in its own user, mount, and network namespace, so it has no
network beyond loopback unless `ci_allow_network=true`, and cannot see or change
the host's mounts.

The network and mount isolation needs **unprivileged user namespaces** enabled
on the host. On Debian they are on by default; confirm with:

```text
sysctl kernel.unprivileged_userns_clone   # 1 means enabled (if the key exists)
cat /proc/sys/user/max_user_namespaces    # a non-zero value
```

If they are disabled, the runner still enforces the timeout, output cap,
resource limits, and scratch directory, but logs a warning that steps run
without network and mount isolation. Enable user namespaces, or do not enable CI
for untrusted work, if that isolation matters to you. macOS builds of the runner
always operate in this degraded mode; run production CI on Linux.

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
