# Read CI results and logs

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
records the run `cancelled`, well before a long step would finish on its own.
A run still queued is cancelled before it starts.

- **Dashboard** — the live run page has a **Cancel run** button. It is the read-
  only dashboard's one mutating action: a loopback-only, same-origin `POST` to
  `/project/<id>/ci/<run-id>/cancel`, which the daemon answers by writing the
  marker directly.
- **Any paired device** — `ckgit ci` works from a checkout of the project (or
  with `--project NAME` anywhere) over the same restricted SSH account as
  `ckgit sync`; no server login is needed:

  ```text
  ckgit ci status                  # CI on or off, latest and active run
  ckgit ci status --all            # the same for every hosted project
  ckgit ci list --limit 10         # runs, newest first, with their ids
  ckgit ci show RUN                # steps, exit codes, detail, artifacts
  ckgit ci log RUN --follow        # the running step's raw log, live, then the next
  ckgit ci watch                   # wait for the newest active run; exit 0 on success, 5 otherwise
  ckgit ci cancel RUN              # preview, confirm, then request the stop
  ```

  Statuses follow the dashboard's rules, judged by the server's clock: a
  running run whose runner has not reported for 90 seconds is shown as
  **interrupted**. `ci cancel` previews the run and asks before it acts
  (`--dry-run` only previews, `--yes` confirms without asking); any paired
  device may cancel any project's run, which is weaker than the push access
  every device already has, and the project page records which device asked.
  `status`, `list`, `show`, and `artifacts` take `--json` for scripts.
  Enabling or disabling CI, rerunning or triggering runs, and secrets stay
  with the administrator on the server.
- **Server** — `ckgit-admin`, as the `ckgit` service account, reads and
  cancels the records directly:

  ```text
  sudo -u ckgit ckgit-admin ci runs   myproject --config /etc/ck-git-hosting/server.ini
  sudo -u ckgit ckgit-admin ci log    myproject <run-id> --follow --config /etc/ck-git-hosting/server.ini
  sudo -u ckgit ckgit-admin ci cancel myproject <run-id> --config /etc/ck-git-hosting/server.ini
  ```

  `ci runs` lists recent runs with status and timing; `ci log` prints a step's
  output and, with `--follow`, streams the running step live; `ci cancel`
  requests cancellation.

`ckgit ci` uses the five `ci-*` control operations documented in the
[SSH and control protocol](../protocol/01-ssh-and-control-v1.md#ci-operations).
On the server, the records are also plain files under the state root:

```text
sudo ls /var/lib/ck-git-hosting/state/ci/runs/myproject
sudo cat /var/lib/ck-git-hosting/state/ci/runs/myproject/<run-id>/run.ini
```
