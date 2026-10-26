---
description: Compile ck-git-hosting from source, run the test suite, and choose where build products go.
---
# Build from source

## Windows builds (port in progress)

The CMake build produces native x64 `ckgit`, `ckdocs`, their tests, and the
AppContainer qualification tools. The connected Windows CI runner is still
under development; the hosting server remains Linux-only.
The version-2 workflow parser supports per-job `runs_on`, dependency lists
(`needs`), and job/step script shells (`sh`, `powershell`, `pwsh`). The push hook now
queues distributed graphs, and the primary runner and protocol-2 gateway
prepare only the assigned child. Protocol 1 cannot claim these children.
The installed Windows runner has completed the native installer job against
the Linux hosting server on rpi4. Full portability qualification remains in
progress; automatic runner startup and broader client journeys are separate checks.

The queue implementation stores parent/child membership and dependency IDs.
A durable graph journal repairs an interrupted enqueue before claims resume;
independent children can run concurrently, failed dependencies skip downstream
jobs, and a parent cancellation reaches queued and running children. Retention
counts a workflow and its children as one run and preserves active graphs.
These storage and scheduling paths have unit tests. The live Windows child
qualification is recorded below; it does not establish every graph recovery case.

Version 2 requires unique artifact names across jobs and an explicit
`pages: {path: public, job: <producer>}` for Pages output. Child results retain
verified bundles without publishing a release or site independently. The
parent result collector waits for every child, verifies the collected bytes,
and combines the outputs; missing or damaged release bundles prevent a
successful release. Local and gateway completion invoke this collector.
Before claiming new work, the primary and gateway retry interrupted parent
publication without repeating already completed child steps. A pending
publication preserves its place in branch order and its retained outputs;
a newer auto-cancelling push also supersedes an unpublished older parent.
Gateway integration tests cover split assignment, upload, combined release,
and recovery. A live combined tag release still needs separate qualification.

Use Visual Studio 2022 Build Tools with the C++ workload, a Windows SDK, and
CMake 3.24 or later and Ninja. Use an x64 Visual Studio Developer PowerShell
so MSVC and the Windows SDK environment are initialized. Install Git for Windows and make `git.exe` available on
PATH for repository inspection and its integration tests. The executables
link the C++ runtime statically.

Runner configuration parsing is shared across platforms and accepts UTF-8
paths and LF or CRLF line endings. Windows build, cache and credential paths
must be absolute; drive-relative paths such as `V:build` are rejected. Windows
also rejects `ci_allow_degraded=true`: there is no unsandboxed fallback. Native
configuration tests cover Unicode paths, spaces and these refusal cases. This
configuration support is also used by the connected Windows runner.
The native Windows test binary also links the shared runner wire protocol:
tests decode a version-2 child assignment, select only that job, encode an
artifact upload and a finish report, and reject a version-1 child envelope.
One-shot runner SSH requests use the shared process implementation, staging
request input in a private directory under the configured build root. The
directory is removed after the request. Native process tests cover binary
input larger than pipe buffers, concurrent output draining, output limits and
timeouts. The persistent log stream now uses the shared `ProcessStream`
interface. Its Windows backend uses a private local named pipe with bounded
asynchronous writes and a Job Object for process cleanup; native tests exercise
bidirectional binary traffic, backpressure, cancellation of pending writes and
handle inheritance. The Unix remote-runner integration test covers checkout,
live logs and output uploads through the same interface. The connected Windows
runner's live checkout/execution qualification is recorded below.
The SSH transport itself is now a separately linked native library. Windows
tests exercise protocol negotiation, claims, binary request input and the log
stream through a local SSH stand-in. Paths are passed as UTF-8, and Git's
`GIT_SSH_COMMAND` quotes executable/configuration paths for its shell. Windows
does not use the Unix ControlMaster socket. These transport tests do not yet
replace the installed runner's separate live connection qualification below.
CI checkout now uses per-child environment overrides instead of the Unix
`env` command. Native Git tests cover fetching a selected commit with depth 1,
a detached checkout, Unicode paths, unchanged committed line endings and
archive mode without `.git`. User/system Git configuration and templates are
disabled. The lease-scoped SSH shim is also native: its integration test runs
real Git pack traffic through a local SSH stand-in that checks the exact job
ID, token and protocol version. Configuration paths include spaces, Unicode
and an apostrophe. The Windows shim inherits only standard I/O and keeps its
SSH child in a kill-on-close Job Object. The connected AppContainer executor
has built this repository's Windows installers from a live rpi4 assignment.
The remote result sink now also builds natively. Its local SSH fixture checks
acknowledged log frames, artifact and Pages upload checksums, and the terminal
report. Relay files live in a private temporary subdirectory that is removed
when the sink closes; failed local writes or short upload reads stop reporting
those bytes as successfully transferred. Pages packaging currently requires
`tar` on the runner's PATH.
The process API also supports synchronous live-output and continuation
callbacks. Live output is delivered beyond the bounded in-memory capture;
callers must cap any logs they retain. A false continuation result cancels the
process tree, including a silent child. Callback exceptions stop the tree
before propagating. Native tests cover cancellation, complete streamed output
and Windows descendant cleanup after cancellation or a log callback failure.
The native Windows executor uses these hooks to stream bounded step logs,
call the result sink's heartbeat and stop on cancellation or lease loss.
Its standalone LPAC fixture exercises pinned source and sister checkouts,
compiler execution in the checkout directory, child-result identity, artifact
packing, Pages handoff and a persistent cache copied into the job. Cache storage
is separated by platform, project, job, toolchain content/configuration and
cache name under `windows-x64-v2/<scope-sha256>/<cache-sha256>`, and locked during use;
only the per-job copy is exposed to LPAC. Tools are copied from an
operator-selected tree and sealed read-only. The native daemon now provides
`serve --remote`, `check` and the Git SSH shim. Its startup qualification covers
protocol negotiation against a local SSH fixture, empty-queue exit, workspace
cleanup and the single-instance lock. The runner installer passes install,
reinstall and uninstall in both scopes, including CurrentUser with a non-admin
token. Automatic startup remains to be implemented and qualified.

The repository's own `.ckgit/ci.yml` now declares a version-2 Linux job and a
`windows-installers` job selected by the `windows` and `x64` labels. The Windows
job runs `packaging/windows/build-ci.cmake` natively: it builds this repository
with MSVC/NMake, invokes Inno Setup for both clients and the runner, creates the portable ZIP,
and places installer files plus input/output hashes in the `windows-installers`
artifact. The Linux job remains the Pages producer. This workflow is the
live rpi4-to-Windows acceptance case. On 2026-10-26, Windows child
`00001793018912592353-4e89d914` succeeded for commit
`8dcf98e5cc33af8074b419b6f02cdf2086b8e363` inside the connected LPAC runner.
Compilation and packaging took 576 seconds. The uploaded `windows-installers`
bundle was downloaded through `ckgit ci download`; its 19,148,800 bytes matched
SHA-256 `2b1859ceb5500146c6e4976774d86367ed5a9ec868fcb329b896fa13ddcc9d14`.
All four package hashes and 291 source-file hashes were verified against the
manifest and exact Git commit. Parent workflow
`00001793018912498671-aea47bb2` completed successfully at 13:36 UTC with both
children successful (48 minutes 17 seconds total). This first live qualification
used the NMake entry point. Its combined artifact list
contains `packages` and `windows-installers`. The Linux bundle was also
downloaded through the parent: 7,454,720 bytes, SHA-256
`5cb8af104c800e3e98c519b45fff77a905d7b0fbe13d12620dbcb0d1e920edb6`.
This was a branch build; live tag-release publication is a separate check.

A second native Windows build of commit `93f70771e500` succeeded as child
`00001793026967533567-8878469f` in 368 seconds. Its artifact contains exactly
three setup executables (ckgit, ckdocs and the runner), the portable ZIP, and
three build/input/package manifests; CPack intermediate files are excluded.
The downloaded bundle is 11,366,400 bytes with SHA-256
`ddbbd2372de10fdc4f3a517c9e2b890993ee465e2d023014d67f15f32711dd9a`.
All four package hashes and 292 source-file hashes matched the exact commit.
The child artifact can be downloaded before the Linux sibling finishes,
including with servers that do not yet support named job selection:

```sh
mkdir -p downloads
ckgit ci download 00001793026967533567-8878469f windows-installers --project ck-git-hosting --into downloads
```

This test artifact expires on 2026-11-02 at 15:09 UTC; it is not a durable
tagged release. Later runs have their own run IDs and retention deadlines.

The Windows job in [GitHub run 40326726111](https://github.com/cklukas/ck-git-hosting/actions/runs/40326726111)
built all three installers and the portable ZIP from commit `1f94b843ca9f`,
passed native tests, and tested both installer scopes. Those exact client
installers were installed on the workstation on 2026-10-26 with a non-admin
token in CurrentUser mode, under `V:\Programs\ckgit` and `V:\Programs\ckdocs`.
The runner was installed with an administrator token in AllUsers mode under
`V:\Programs\ck-git-runner`. Installed executable hashes matched the CI ZIP,
and the executables reported `0.8.2+g1f94b843ca9f`. The installed runner then
negotiated protocol 2 with rpi4 and exited successfully on an empty queue.

On the development workstation, keep sources in a permanent directory on
`V:` and select a unique temporary directory before running development tools:

```powershell
if (!(Test-Path V:\)) { throw 'Development drive V: is unavailable' }
$scratch = Join-Path V:\tmp ('ckgit-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $scratch | Out-Null
$env:TEMP = $scratch; $env:TMP = $scratch; $env:TMPDIR = $scratch
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
cpack --config "$scratch\ckgit-ninja-release\CPackConfig.cmake" -C Release -B "$scratch\dist"
```

The native AppContainer runner uses the separate `windows-lpac-release`
preset with NMake. Ninja is supported for development and GitHub builds;
its current LPAC limitations are described under [Windows runner](../operations/ci-remote-runners.md#windows-runner).

Check each command's exit code before continuing. Supply
`-DCKGIT_BUILD_VERSION=<exact-version>` when configuring a release; the default
is the version in `VERSION` with a `+source` suffix.

Native tests cover private client configuration and checkout selection files,
process locks, Git repository discovery (including Unicode paths and linked
worktrees), process execution, file serving, diagrams, and the docs preview.
These tests also run without an administrator token and exercise the native
CLI's setup, configuration and scan commands. Broader remote Windows command
journeys require additional end-to-end qualification.

Inno Setup 6 builds `packaging/windows/ckdocs.iss` and
`packaging/windows/ckgit.iss` with explicit
`/DBinaryDir`, `/DOutputDir`, and `/DReleaseVersion` definitions. The installer
supports `/CURRENTUSER` without elevation and `/ALLUSERS` with administrator
rights. It tracks its own PATH entry and preserves user files on uninstall.
The Ready to Install page and setup log report `git.exe` and `ssh.exe` files
found in absolute directories on the installer's PATH, including quoted paths
and portable Git directories. This preflight checks presence, not executable
version or trust, and does not run discovered programs with installer rights.
Missing tools do not block local documentation use or offline installation;
remote operations require Git and SSH, and ref portals require Git. Existing
client/runner configuration remains unchanged. Run `ckgit doctor` after client
setup to check Git availability and the configured server connection.

The combined `ckgit` installer offers `ckgit`, `ckdocs`, or both (the default).
For unattended selection, pass `/COMPONENTS="ckgit"`, `/COMPONENTS="ckdocs"`,
or `/COMPONENTS="ckgit,ckdocs"`. An empty selection is rejected. Reinstalling
can add a component; deselecting a previously installed component does not
remove its existing files. Use the uninstaller to remove the installation.
`tests/integration/windows_installer.ps1` checks installation, reinstallation,
binary identity, execution and uninstallation. For `ckgit`, it also rejects an
empty selection and upgrades the initial selection to both components. Set
`-InitialComponents ckgit` or `-InitialComponents ckdocs` to test a single-tool
installation; the default tests the installer's default selection.
Use `-RequireNonAdmin` from a
non-elevated process to verify installation without administrator rights.
Pass `-Product ckgit -DocsBinary <verified-ckdocs.exe>` for the client installer;
it bundles both executables. The portable ZIP also contains both tools.

The reusable GitHub Windows workflow builds the ZIP and all three installers,
runs native tests and checks both installer scopes, including every client
component selection. Its callers include push CI and the
release workflow. Hosted runners use their supplied `RUNNER_TEMP`; the `V:`
policy above applies to the development workstation. Installers are currently
unsigned.

### AppContainer qualification

`ckgit-lpac-probe` explicitly exercises the sandbox with staged compiler,
CMake and NMake binaries. Pass the temporary parent, MSVC Hostx64/x64 directory,
CMake directory and `kernel32.lib` path, in that order.
It is separate from CTest because it creates Windows-managed
AppContainer profiles in the user's profile directory. Job data, tools and
temporary output stay beneath the explicit test directory on `V:`.

The current backend requires an administrator broker token to create a named,
private Window Station. Job processes still run in LPAC, with their own
invisible desktop, restricted file access and no network capability. There is
no fallback to the user's interactive desktop or unsandboxed execution.
This desktop supports initialization of Windows libraries used by console
tools; it does not launch Explorer or display a graphical session.

The qualification covers compiler/linker execution, a minimal native program,
a CMake/NMake build, live output, cancellation and isolation checks. It does not establish compatibility
with every SDK, build system or installer tool; the connected runner's full
installer qualification is recorded separately above. Non-admin ckdocs installation is independent of this
broker requirement.

For a small path-compatibility diagnostic without staging the compiler, run:

```powershell
ckgit-lpac-probe.exe --path-diagnostics V:\tmp\YOUR-TASK
```

Keep `ckgit-job-launcher.exe` beside the probe. This mode creates one disposable
LPAC profile and workspace, stages only these two executables, and reports the
results and Win32 error codes for file attributes, full/long path conversion,
and directory changes. It does not grant access to workspace ancestors.
On the workstation, querying attributes and changing into the job's own work
directory succeeded, while `GetLongPathNameW` failed with error 5 (access denied).
The ancestors outside the sandbox were inaccessible. Microsoft documents that
[GetLongPathNameW may require access to parent directories](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew),
even when the target is accessible. This is consistent with the absolute-path
failures observed in Ninja's `cmd.exe` commands; it is not a successful Ninja
qualification. The probe removes its profile and workspace on normal exit.

## Compile and test

The source tree is never used for build products; every build directory must
lie beneath a build root, and the test suite keeps every scratch file there
too. This is the portable form, explicit about both and safe to copy verbatim
onto another machine or into another project's own CI:

```text
make BUILD_ROOT=/tmp/ck BUILD_DIR=/tmp/ck/build all check
```

For everyday local use, `BUILD_ROOT` defaults to your system's temporary
directory (`TMPDIR`, or `/tmp`), so a bare `BUILD_DIR=` works unmodified on a
fresh clone; choose a new directory under the root for each build:

```text
make BUILD_DIR=/tmp/ck-git-hosting-my-build
make BUILD_DIR=/tmp/ck-git-hosting-my-build check
```

To pin a different default for one particular checkout instead of typing
`BUILD_ROOT=` every time — for example to keep build products off a
network-mounted source volume — copy `local.mk.example` to `local.mk`
(gitignored) and set `BUILD_ROOT` there. `BUILD_ROOT` may name several
approved roots separated by spaces, such as two scratch volumes; `BUILD_DIR`
can then be beneath any of them:

```text
BUILD_ROOT := /Volumes/S8T/tmp /Volumes/PRO-BLADE/tmp
```

`ckgit --version` identifies the release version and Git revision, including
tracked changes. A source archive with no `.git` (a `git archive` export, or
this project's own self-hosted CI checkout) instead reads the commit
`export-subst` stamps into `.ckgit/build-commit`, falling back to a plain
`+source` suffix only if that is unavailable too. Packaging can supply
`CKGIT_BUILD_VERSION=0.1.0` or another exact build identifier to `make`.

This project builds, tests, and releases itself through its own CI and
release mechanism — see
[Deploying ck-git-hosting's own release](../operations/releases.md#deploying-ck-git-hostings-own-release)
and the [self-hosted release and deploy guide](../operations/05-releases-and-deploy.md)
for the end-to-end flow from a pushed tag to an upgraded server.
