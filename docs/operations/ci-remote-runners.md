# Share CI load with remote runners

A ck-git-hosting server runs CI jobs itself, with `ck-ci-runnerd` on the same
machine. A second Linux machine — a desktop, a laptop that is not always on —
can take load off it: installed as a **remote runner**, it reaches the server
over SSH with its own restricted key, claims queued jobs, runs them in the
same sandbox, and sends everything back. Results live only on the server; the
dashboard, `ckgit ci`, release downloads and Pages work exactly as for a job
the server ran, and nothing ever connects to the runner.

## How jobs are shared

- **Secondary runners first.** A remote runner claims a new job the moment it
  is queued. The server's own runner (the *primary*) claims a job only after
  it has waited `ci_claim_delay_ms` (default 1000 ms) without a remote runner
  taking it, so a free remote runner always wins, and with every remote runner
  busy or offline the server simply runs the job itself, one second later.
- **Several remote runners compete.** Each runs one job at a time and asks for
  work only when idle; the first to claim a job gets it.
- **One run per project and branch at a time.** Runs of one ref finish in push
  order, so an older commit can never publish after a newer one. Different
  projects and branches run in parallel.
- **No polling.** Every runner is woken by a change notification on the
  server's queue: the primary watches it directly (inotify), and a remote
  runner holds a long poll that the server answers the moment a job appears.
- **Every kind of build runs anywhere.** Branch, tag (release) and Pages builds
  are scheduled the same way.

## What happens when a runner goes away

A claimed job carries a **lease** that the runner renews while it works (every
15 s by default; the lease lasts `ci_lease_seconds`, default 90 s). When a
runner goes to sleep, loses the network or crashes, its heartbeats stop, the
lease lapses, and the server puts the job back into the queue as its next
attempt: the dashboard shows it Pending again, the lost attempt's step logs are
kept aside on the server, and the next free runner — or the server itself —
runs it. After `ci_max_attempts` (default 2) a job whose runner keeps getting
lost is recorded as an error. A runner that is stopped cleanly (`systemctl
stop`, shutdown) hands its job back at once instead of waiting for the lease.
A runner that wakes up after its job was requeued learns that its lease is gone
and discards its work; its results can never overwrite the newer attempt.

## What a runner sends back

| Result | Sent when | Stored on the server as |
|---|---|---|
| Run status and step results | live while running, final at the end | the run record |
| Step logs | live, roughly every 250 ms — also for failed, cancelled and timed-out runs | the step logs, capped by the server's `ci_max_log_bytes` |
| CI artifacts | after every step of the job passed | artifacts, with retention |
| Release assets | only when the whole tag build succeeded | the release |
| Pages site | only for a successful default-branch build, never over a newer live site | a new site version |

Uploads are checksummed and resumable: after a dropped connection the runner
continues where the server's bytes end, and nothing is published until the run
finishes and every upload is verified. A dropped log stream is resumed the same
way, without duplicate lines.

**The runner keeps nothing.** Its build root is emptied when it starts and after
every job — success, failure, cancel, a lost lease: the clone with its `.git`,
every sister, everything the steps built, and the packed bundles. Declared
`cache:` directories are per run on a remote runner unless its configuration
sets `ci_cache_root`.

## Live view

The dashboard's live log follows a remote run exactly like a local one,
typically less than a second behind: the runner pushes its progress into the
run record and step logs on the server, which is all the dashboard ever reads.
A run that is requeued ends the live view and the page reloads onto the next
attempt. The run list and the run page say which runner ran a job, and the
**Runners** page (`/ci/runners`) lists every runner, its last contact, version
and current job.

## Set up a remote runner

On the runner machine (Debian/Raspberry Pi OS with unprivileged user namespaces,
as for the server's own runner — see
[the sandbox requirements](ci-runner.md#the-sandbox-and-its-requirements)):

```text
sudo apt install ./ck-git-runner_VERSION_ARCH.deb
sudo ck-git-runner register --server ckgit@rpi4 --name uconsole
```

The package adds the `ckgit-runner` account, its home `/var/lib/ck-git-runner`
and the `ck-git-runner.service` unit (installed disabled); it never turns the
machine into a second server. `register` creates the runner's SSH key, pins the
server's host key, writes `/etc/ck-git-hosting/runner.ini`, and prints the two
commands that follow.

On the server, admit the runner's key (it is printed by `register`):

```text
sudo -u ckgit ckgit-admin runner add uconsole --public-key uconsole.pub \
  --config /etc/ck-git-hosting/server.ini | sudo tee -a /etc/ck-git-hosting/authorized_keys
```

Then start the runner:

```text
sudo systemctl enable --now ck-git-runner.service
journalctl -u ck-git-runner.service -f
```

### `runner.ini`

```text
schema_version=1
server=ckgit@rpi4                 # the server's SSH destination
name=uconsole                     # the runner's name (its key's client id)
ci_build_root=/var/lib/ck-git-runner/ci-build
identity=/var/lib/ck-git-runner/.ssh/id_ed25519
known_hosts=/var/lib/ck-git-runner/.ssh/known_hosts
# claim_wait_seconds=60           # long-poll length (1..120)
# ci_allow_network=false          # may only tighten the server's policy
# ci_timeout_seconds=600          # may only tighten the server's policy
# ci_cache_root=...               # keep cache: directories (opts out of a clean runner)
# ci_allow_degraded=false         # true: run without the namespace sandbox
```

The server's limits — step timeout, log cap, artifact cap, network — are
authoritative; a runner can only tighten them.

### Server settings

In the server's `server.ini`:

```text
# ci_claim_delay_ms=1000      # the primary leaves a new job this long for a free remote runner (0..60000)
# ci_runner_labels=arm64,linux # the server's own runner's labels for runs_on: (besides its name, primary)
# ci_lease_seconds=90         # a claim lapses without renewal after this long (10..3600)
# ci_max_attempts=2           # attempts before a job whose runner keeps getting lost is an error (1..10)
# ci_rescan_seconds=300       # safety rescan behind the queue notifications (10..3600)
```

## Manage runners

```text
sudo -u ckgit ckgit-admin runner list --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin runner disable uconsole --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin runner enable uconsole --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin runner remove uconsole --config /etc/ck-git-hosting/server.ini
```

`disable` drains a runner: it finishes its current job but claims no new one.
`remove` deletes its record; delete its line from `authorized_keys` to revoke
its key.

### Labels

A workflow can ask for a kind of runner with
[`runs_on:`](06-ci-yml-reference.md#choose-a-runner-runs_on), such as a Mac
for macOS builds or a machine with a big disk. Give runners labels on the
server:

```text
sudo -u ckgit ckgit-admin runner labels mac-mini macos,arm64 --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin runner labels uconsole - --config /etc/ck-git-hosting/server.ini
```

The first command sets a runner's labels, and `-` removes them all. `runner
add NAME --public-key FILE --labels a,b` sets them when the runner is
registered. Set `ci_runner_labels` in `server.ini` for the server's own runner
and restart `ck-ci-runner.service`. A runner's name always counts as a label
too. `runner list` and the dashboard's Runners page show every runner's
labels.

Labels come only from the server's records, never from the runner, so a
runner cannot take jobs meant for another kind of machine by claiming a label.

## Security

- **A separate key class.** A runner key's forced command carries
  `--runner-config`; `ck-git-shell` then accepts only the runner protocol
  (`ckgit-runner 1 …`, see the
  [protocol reference](../protocol/01-ssh-and-control-v1.md#runner-keys)) and
  hands it to the gateway — no Git push, no `ckgit-rpc`. A device key cannot
  run any runner operation.
- **Lease-scoped access.** A runner can fetch only the project of the job it
  holds and the sisters that job's workflow declares, and only while its lease
  is valid; every write carries the lease's token.
- **Trust.** A remote runner receives the source of every CI-enabled project
  (and their sisters) and reports results for the jobs it claims. Use only
  machines you trust as much as the server.
- **Its own secrets stay hidden.** Steps run in the same sandbox as on the
  server; the runner's SSH key directory is masked from them. Without
  unprivileged user namespaces a remote runner refuses to run jobs at all
  unless `ci_allow_degraded=true`.
