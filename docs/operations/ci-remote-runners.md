---
description: "Register and manage remote CI runners, including the native Windows AppContainer runner."
---
# Share CI load with remote runners

A ck-git-hosting server runs CI jobs itself, with `ck-ci-runnerd` on the same
machine. A second Linux machine — a desktop, a laptop that is not always on —
can take load off it: installed as a **remote runner**, it reaches the server
over SSH with its own restricted key, claims queued jobs, runs them in the
same sandbox, and sends everything back. Results live only on the server; the
dashboard, `ckgit ci`, release downloads and Pages work exactly as for a job
the server ran, and nothing ever connects to the runner.

## How jobs are shared

Remote runners negotiate protocol version 2 before claiming work. POSIX runners
can fall back to version 1 when connecting to an older server; Windows runners
require version 2 and refuse legacy-only servers before claiming work. Version 2 permits individual
jobs from a workflow with `version: 2` to run on different labelled runners;
version-1 runners only receive legacy workflows. The negotiated version stays
fixed throughout a lease, including source checkout, logs and artifact uploads.
Failed greetings do not authorize a claim. After a failed claim, the runner
negotiates again before retrying. Restart runners after upgrading an older
server so existing version-1 connections can use distributed jobs.

- **Secondary runners first.** A remote runner claims a new job the moment it
  is queued. The server's own runner (the *primary*) claims a job only after
  it has waited `ci_claim_delay_ms` (default 1000 ms) without a remote runner
  taking it, so a free remote runner always wins, and with every remote runner
  busy or offline the server simply runs the job itself, one second later.
- **Several remote runners compete.** Each runs one job at a time and asks for
  work only when idle; the first to claim a job gets it.
- **One run per project and branch at a time.** New branch pushes cancel older
  pending/running runs by default. Set `auto_cancel: false` in the existing
  `.ckgit/ci.yml` to retain every run in push order. The replacement waits for
  cancellation to finish; an older commit cannot publish after a newer one.
  Tags are preserved. Different projects and branches run in parallel.
- **No polling.** Every runner is woken by a change notification on the
  server's queue: the primary watches it directly (inotify), and a remote
  runner holds a long poll that the server answers the moment a job appears.
- **Every kind of build runs anywhere.** Branch, tag (release) and Pages builds
  are scheduled the same way.

## Windows runner

Windows uses the native `ck-ci-runnerd.exe` broker and an LPAC/AppContainer
for every job. Each executing step owns a fresh, invisible Window Station
and desktop, released after its process tree stops. An elevated broker can
choose the station's name; an ordinary user can use a name assigned by Windows
when that logon session does not already own the corresponding station.
Build tools remain restricted; no
interactive desktop or Explorer session is started. Network access is denied,
even when the server would allow it, and there is no degraded execution mode.
Here, “desktop” means a Windows system object, not a graphical build interface.
Console toolchains can load USER32 through their dependencies; Windows then
connects the process to a Window Station and desktop during initialization.
The private objects let those dependencies initialize without sharing the
user's desktop. See Microsoft's [process connection documentation](https://learn.microsoft.com/en-us/windows/win32/winstation/process-connection-to-a-window-station).
The broker restores both its process station and its thread desktop after
creating these objects. It allows up to two seconds for Windows to release
references left by an exited process, but every attempt requires a **new**
station. It never reopens an existing station or changes its permissions.
Run `ck-ci-runnerd check --config RUNNER.INI` under the intended account and
logon session before starting the runner. Some SSH/service logon sessions
already own the OS-assigned station; that conflict fails before a job is
claimed. Multiple overlapping steps in one such standard-user logon session
are not supported.
Windows-managed AppContainer profiles use the operating system's profile
location. Checkouts, staged tools, caches and logs use the configured paths.

Windows artifact packing uses the bundled, statically linked libarchive 3.8.10;
it does not invoke `tar.exe`. It writes uncompressed TAR files, streams regular
files from validated handles and rejects links or reparse points. Packing keeps
the configured byte cap, cancellation checks and five-minute time limit, with
an additional limit of 100,000 entries. Failed packing removes partial output.
The broker performs this work after the job's processes have terminated.
Pages packing also uses the library on both platforms, omitting links and
special files, with the Pages upload cap and a ten-minute packing deadline.
Portal Git-ref trees and server-side Pages uploads are also extracted with
the pinned library, without a `tar` executable. Extraction uses a new private
directory and removes partial output on failure. It rejects absolute paths,
parent traversal, duplicate files and sparse entries, and never creates links
or restores archive ownership or permissions. Portal trees omit links; Pages
uploads reject them. Input and extracted content are capped at 8 GiB, individual
files at 1 GiB, with at most 100,000 entries (plus the Pages TAR root).
Portal extraction has a two-minute deadline; Pages extraction has ten minutes.

The `ck-git-runner` installer includes the broker, `ckgit-job-launcher.exe` and
`runner.ini.example`. CurrentUser installs its files without administrator
rights; broker startup also depends on the logon-session check above.
AllUsers installation needs administrator rights. Installing the package does not automatically start a
runner or register its SSH key. Closing an idle Windows runner interrupts its
SSH negotiation or pending claim, stops that SSH process and removes the
session workspace. Installer upgrades of this idle state are checked in both
installation scopes. A service upgrade during an active compiler invocation
was also tested: the runner handed the job back, restarted, and completed a
fresh attempt. Another fixture upgrade began after an artifact upload request
was observed; the job was handed back and the second attempt published a
checksum-verified 64 MiB artifact. The measurement did not establish that a
network request was still in flight at the instant of service stop, so wait
for jobs to finish before upgrading in production.
The transport now interrupts a silent stream greeting or an in-flight artifact
upload when the broker receives a stop request. Upload retry delays also check
for shutdown, cancellation and lease loss, and interrupted bytes are not
declared as a successful artifact. Log acknowledgement waits, final-result
requests and retry delays also observe shutdown. A final hand-back request has
a two-second timeout; the runner reports a confirmed hand-back only after the
host acknowledges it. Otherwise it reports the uncertainty and the host's
lease recovery remains responsible for the job. Native process tests cover
silent greetings, uploads, log acknowledgements, finish and hand-back requests.
Artifact checksum calculation also checks cancellation and pumps heartbeats
between 64 KiB reads; a blocked filesystem read itself is not interrupted.
Use a short `ci_build_root` on Windows: the runner adds session and job
directories, and compiler-generated paths can exceed MSVC's limits. In the
service qualification, a deeply nested temporary root produced C1083 while
creating a libarchive object; a retry uses a shorter root on the same drive.
The active-build and upload upgrade tests used installers with unchanged runner
binary bytes; they do not establish compatibility across arbitrary versions.

The Windows service host is under qualification. Its `service --config PATH
--log PATH --service-name NAME` entry point connects to the Service Control
Manager and requires the matching non-administrator `NT SERVICE\NAME` virtual
account. It does not accept LocalSystem or register itself automatically.
The runner package includes `register-service.ps1` for an explicit,
administrator-initiated registration. It creates a stopped, manual-start
service and refuses to replace an existing name. Supply absolute paths for
`-Runner`, `-Config`, and `-Log`, and optionally a `-Name`. Before starting,
the administrator must grant the resulting `NT SERVICE\NAME` account read
access to the executable, configuration, SSH identity, known-hosts file and
toolchain, and the required write access to its build, cache and log paths.
Keep these permissions restricted to the service account and administrators;
the script does not modify ACLs or validate effective access. Service setup
and full compiler execution remain under qualification.
The supervisor starts a persistent worker with the same local identity and a
new logon session, explicitly attached to the parent's noninteractive desktop.
The worker can then create fresh per-step Window Stations. No existing station
ACLs are broadened. Windows network credentials are deliberately unusable;
remote repository access continues to use the configured SSH identity.

Private events carry readiness and stop signals. Readiness follows a real
LPAC/USER32 process self-check, also performed by `check` and console `serve`.
The supervisor guards the worker process tree; LPAC roots are created atomically
in their own UI-restricted Job Objects because Windows does not allow those UI
restrictions in nested jobs. Build processes cannot break away from their jobs.
A worker that ignores shutdown is terminated after ten seconds and is reported
as an error. Recorded workspaces support cleanup on the next start.
The worker writes `--log`; the supervisor appends `.host.log` to that path.

The opt-in `tests/integration/windows_service.ps1` test takes `-Runner`,
`-FakeSsh` (the built `ckgit-process-tests.exe`) and an existing `-TestRoot`.
It requires an administrator to create a disposable virtual-account service,
checks the running worker's SID and executable, stops it, verifies workspace
and process cleanup, and removes the service. The test passed on 2026-11-01;
the persistent worker stopped in 0.262 seconds. The normal process suite and
LPAC path-isolation regression also passed after the atomic Job Object change.
Add `-ExerciseCrashRecovery` to exercise normal stop, worker termination,
supervisor termination and a final normal stop using the same work root.
Both crash/restart runs passed: no fixture processes survived, old sessions
were recovered on restart, and the final stop removed all session directories.
These are idle-runner tests with a protocol fixture, not active compiler tests.
The same registration and crash-recovery fixture passed with the installed
GitHub runner from commit `8cc509f`. A separate service test connected to the
real host and built this repository's Windows installers under LPAC with the
short build root; its four packages and source fingerprints were verified.
ACL provisioning remains manual. For AllUsers upgrades, the installer stops
running virtual-account services whose configured executable is exactly the
runner in the installation directory, then restarts them. Setup exit code 20
means the package was installed but at least one service did not restart;
inspect the setup log and service logs. Tests cover idle services, an unrelated
service and a disabled service that cannot restart. The active-compiler
fixture completed a real retried LPAC job and verified its four packages.
An idle installed service was then upgraded between different GitHub builds,
restarted with its configuration intact, and completed another full LPAC build.
A separate upload-stage upgrade returned the job and successfully retried it;
the exact stop instant relative to an in-flight network request was not proven.

Use a dedicated runner identity with the
server's restricted runner key, and pin the server's host key in `known_hosts`.
The example selects Git for Windows' `usr/bin/ssh.exe` explicitly. On the
qualification workstation, Windows' built-in OpenSSH returned protocol replies
but hung at exit with redirected streams; Git for Windows' SSH completed those
same exchanges. Adjust `ssh_program` if Git is installed in another location.

On the development workstation, install to `V:\Programs\ck-git-runner` and
keep configuration, credentials and the staged toolchain beneath a protected
permanent directory such as `V:\ProgramData\ckgit-runner`. Verify `V:` exists
before invoking development tools. Select a unique `V:\tmp` directory and set
`TEMP`, `TMP` and `TMPDIR` to it before staging tools.

`packaging/windows/build-lpac-ninja.ps1` builds the pinned Ninja 1.13.2 source
archive offline, applying the `LOCAL` named-pipe namespace required by LPAC.
Supply absolute `-SourceArchive`, `-TemporaryRoot`, `-Destination` and
`-CmakeProgram` paths. The script requires Visual Studio 2022 x64 tools,
verifies the archive and patched source hashes, and retains the executable,
license, build logs and provenance record. Obtain the upstream `v1.13.2` source
archive before provisioning; no download occurs inside the job.

`packaging/windows/stage-toolchain.ps1` copies explicitly selected MSVC,
Windows SDK, CMake, Inno Setup and that Ninja package into a new toolchain.
Supply `-Destination`, `-TemporaryRoot`, `-MsvcRoot`, `-SdkRoot`, `-SdkVersion`,
`-CmakeRoot`, `-InnoRoot` and `-NinjaRoot` (the previous script's destination).
Existing destinations are refused. Every copied file is checked against its
source SHA-256; installed tool permissions are unchanged. The optional
`vctip.exe` telemetry uploader is omitted from the staged copy so resident
uploaders cannot exhaust the job's process limit. Put `ninja` before
`cmake/bin` in `ci_toolchain_paths`, as in the example configuration.

The repository's installer job uses the `windows-release` CMake preset
(Ninja), also used by development and GitHub CI. The older
`windows-lpac-release` NMake preset remains available for explicit diagnostics;
there is no automatic fallback. Updating an existing runner requires staging
the LPAC-compatible Ninja before using this workflow.

The runner copies tools into its private `tool files` directory. The space
makes CMake quote absolute tool paths: unquoted absolute executable paths in
`cmd.exe` failed in LPAC, while quoted paths worked. An initial full
ck-git-hosting Ninja build produced all three installers and the portable ZIP
under an ordinary filtered UAC token (381 seconds; 301 source hashes and all
four executable timestamps verified). A second full build with the integrated private tool-directory change
and a build root without spaces passed in 361 seconds, with the same
source/package/timestamp checks. Final staged-toolchain deployment is tracked
in the validation record. Absolute `cd /D`
commands still failed in the diagnostic; this is not a claim that arbitrary
Windows build scripts work in LPAC. Do not broaden filesystem permissions.

An additional ordinary-user and administrator diagnostic mapped the existing
workspace to a temporary, randomly named DOS device without changing drive
letters or filesystem ACLs. `GetLongPathNameW` succeeded through that alias,
but `cmd.exe` rejected the device path as a UNC current directory. This does
not solve Ninja compatibility. Both aliases and workspaces were removed; the
experiment is not part of the runner.

The example configuration matches this staged directory layout:

- `ci_toolchain_root`: absolute path to the operator-controlled tool tree.
- `ci_toolchain_paths`: semicolon-separated relative binary directories, using
  forward slashes. Absolute paths and parent traversal are rejected.
- `ci_toolchain_env.NAME`: explicit uppercase environment assignments. The
  literal `${TOOLS}` expands to the current job's read-only tool copy.

From the account and logon session intended for the runner, check the
configuration before serving. The configuration, SSH key, toolchain and build
directory must be accessible to that account:

```powershell
& 'V:\Programs\ck-git-runner\ck-ci-runnerd.exe' check --config 'V:\ProgramData\ckgit-runner\runner.ini'
& 'V:\Programs\ck-git-runner\ck-ci-runnerd.exe' serve --remote --config 'V:\ProgramData\ckgit-runner\runner.ini'
```

`--once` drains available jobs and exits when the queue is empty. The build
root has an exclusive instance lock. Each invocation owns a private session
workspace; each job has a separate AppContainer and tool copy. Caches are
separated by platform, project, job, toolchain fingerprint and cache name under
`windows-x64-v2/<scope-sha256>/<cache-sha256>`. The fingerprint covers the sealed
tool files, executable search order and configured toolchain environment.
Names are hashed with case preserved; Windows case folding cannot merge them.
Changing tools or their configuration starts a fresh cache. Older unscoped
Windows caches are not imported. Each cache is copied into the job while
the persistent cache is locked. Lease monitoring continues during tool staging,
Git checkout and build steps. The repository's `windows-installers` job uses
CMake directly and does not need PowerShell inside the sandbox; other script
jobs need their selected shell in the staged toolchain.

Configure the server's primary runner with `ci_runner_labels=linux,arm64`
when it runs on an ARM64 Linux host, and register the Windows runner with
`--labels windows,x64`. The repository's own version-2 workflow then schedules
its Linux build and native Windows installers independently from the same push.
See [the distributed workflow example](06-ci-yml-reference.md#build-on-linux-and-windows-from-one-push).

## What happens when a runner goes away

A claimed job carries a **lease** that the runner renews while it works (every
15 s by default; the lease lasts `ci_lease_seconds`, default 90 s). When a
runner goes to sleep, loses the network or crashes, its heartbeats stop, the
lease lapses, and the server puts the job back into the queue as its next
attempt: the dashboard shows it Pending again, the lost attempt's step logs are
kept aside on the server, and the next free runner — or the server itself —
runs it. After `ci_max_attempts` (default 2) a job whose runner keeps getting
lost is recorded as an error. A runner that is stopped cleanly (`systemctl
stop`, shutdown) attempts to hand its job back immediately. If the host cannot
confirm that request, recovery follows the lease policy instead.
A runner that wakes up after its job was requeued learns that its lease is gone
and discards its work; its results can never overwrite the newer attempt.

On Windows, closing the broker's Job Object handles also terminates its build
processes after an abrupt broker exit. Before claiming another job, the runner
recovers abandoned sessions under its configured build root. Each session and
AppContainer workspace has a flushed, owner-only resource record, held open
exclusively while in use. Recovery checks the record's Windows owner, resource
name and format before deleting that user's recorded profile and workspace.
Live, unrecorded, malformed and explicitly retained workspaces are preserved.
Directory reparse points are removed without following their targets. Failed
cleanup keeps its record for another attempt; a recovery error prevents new
job claims. Older runner directories without these records are not reclaimed
automatically.

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

**Job workspaces are temporary.** Normal cleanup removes the clone with its
`.git`, every sister, build outputs and packed bundles after success, failure,
cancel or a lost lease. Windows startup recovery is limited to its recorded
resources as described above; it does not empty unrelated directories. Declared
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

Runners registered with the `windows` label never claim unlabelled jobs.

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

## Reported execution properties

New runners negotiate `execution_report=1` with the server before sending
structured execution properties. Older servers receive the existing finish
payload unchanged. The Windows runner records LPAC, the actual architecture,
denied network access, a 4 GiB memory limit and 64-process limit per step, and
the configured step timeout after a process has run with those protections.
Setup failures before process launch leave these properties unknown.

The server stores the report with the job attempt. `ckgit ci show RUN --job NAME`
and the job's dashboard page display it as **runner-reported**, not independent
attestation. JSON exposes it as `execution`; missing reports are `null`.
Old run records remain readable. New records containing a report use schema 5;
rollback to a server predating that schema requires restoring compatible state,
not merely replacing its executable. Parent workflows have no shared sandbox;
inspect their individual jobs for execution properties. Linux and macOS also report the child process's actual setup, through a private
channel closed before executing job code. A successful namespace preflight does
not substitute for the child's own result. Linux reports `linux-namespaces`
only after namespace creation succeeded, with `loopback-only`, `denied` or
`unrestricted` network access as applied. This does not independently attest
every filesystem masking mount. Existing degraded execution is explicitly
reported as `none` with `unrestricted` networking; macOS development execution
uses that mode too. No new isolation fallback is introduced.

Memory/process limits of zero mean no such cap was configured. POSIX's separate
core-dump and single-file-size limits are not represented by those fields. If
steps report different execution properties, the run-level report is cleared
rather than claiming one policy covered them all.
