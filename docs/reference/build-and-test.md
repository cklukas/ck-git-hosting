---
description: Compile ck-git-hosting from source, run the test suite, and choose where build products go.
---
# Build from source

## Windows builds (port in progress)

The CMake build produces native x64 `ckgit`, `ckdocs`, their tests, and the
AppContainer qualification tools. The connected Windows CI runner is still
under development; the hosting server remains Linux-only.
The version-2 workflow parser supports per-job `runs_on`, dependency lists
(`needs`), and script shells (`sh`, `powershell`, `pwsh`). The push hook now
queues distributed graphs, and the primary runner and protocol-2 gateway
prepare only the assigned child. Protocol 1 cannot claim these children.
The production Windows runner and the final deployment qualification remain
under development; continue using version 1 for deployed workflows until
those checks are complete.

The queue implementation stores parent/child membership and dependency IDs.
A durable graph journal repairs an interrupted enqueue before claims resume;
independent children can run concurrently, failed dependencies skip downstream
jobs, and a parent cancellation reaches queued and running children. Retention
counts a workflow and its children as one run and preserves active graphs.
These storage and scheduling paths have unit tests; they do not yet constitute
the rpi4-to-Windows end-to-end qualification.

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
and recovery; the actual Windows workstation qualification is still pending.

Use Visual Studio 2022 Build Tools with the C++ workload, a Windows SDK, and
CMake 3.24 or later. Install Git for Windows and make `git.exe` available on
PATH for repository inspection and its integration tests. The executables
link the C++ runtime statically.

Runner configuration parsing is shared across platforms and accepts UTF-8
paths and LF or CRLF line endings. Windows build, cache and credential paths
must be absolute; drive-relative paths such as `V:build` are rejected. Windows
also rejects `ci_allow_degraded=true`: there is no unsandboxed fallback. Native
configuration tests cover Unicode paths, spaces and these refusal cases. This
configuration support does not yet constitute a connected Windows runner.
The native Windows test binary also links the shared runner wire protocol:
tests decode a version-2 child assignment, select only that job, encode an
artifact upload and a finish report, and reject a version-1 child envelope.

The Windows job in [GitHub run 40305821882](https://github.com/cklukas/ck-git-hosting/actions/runs/40305821882)
built both installers and the portable ZIP from commit `ae982d1d6295`, passed
native tests, and tested both installer scopes. Those exact installers were
installed on the development workstation on 2026-10-26 with a non-admin token
in CurrentUser mode, under `V:\Programs\ckgit` and `V:\Programs\ckdocs`.
Installed executable hashes matched the CI ZIP, and both reported
`0.8.2+gae982d1d6295`. This verifies client installation; runner installation
and the rpi4-to-Windows job qualification remain outstanding.

On the development workstation, keep sources in a permanent directory on
`V:` and select a unique temporary directory before running development tools:

```powershell
if (!(Test-Path V:\)) { throw 'Development drive V: is unavailable' }
$scratch = Join-Path V:\tmp ('ckgit-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $scratch | Out-Null
$env:TEMP = $scratch; $env:TMP = $scratch; $env:TMPDIR = $scratch
cmake -S . -B "$scratch\build" -G 'Visual Studio 17 2022' -A x64
cmake --build "$scratch\build" --config Release --parallel 4
ctest --test-dir "$scratch\build" -C Release --output-on-failure
cpack --config "$scratch\build\CPackConfig.cmake" -C Release -B "$scratch\dist"
```

Check each command's exit code before continuing. Supply
`-DCKGIT_BUILD_VERSION=<exact-version>` when configuring a release; the default
is the version in `VERSION` with a `+source` suffix.

Native tests cover private client configuration and checkout selection files,
process locks, Git repository discovery (including Unicode paths and linked
worktrees), process execution, file serving, diagrams, and the docs preview.
These tests also run without an administrator token and exercise the native
CLI's setup, configuration and scan commands. Remote Windows command journeys
and the connected runner require additional end-to-end qualification.

Inno Setup 6 builds `packaging/windows/ckdocs.iss` and
`packaging/windows/ckgit.iss` with explicit
`/DBinaryDir`, `/DOutputDir`, and `/DReleaseVersion` definitions. The installer
supports `/CURRENTUSER` without elevation and `/ALLUSERS` with administrator
rights. It tracks its own PATH entry and preserves user files on uninstall.
`tests/integration/windows_installer.ps1` checks installation, reinstallation,
binary identity, execution and uninstallation. Use `-RequireNonAdmin` from a
non-elevated process to verify installation without administrator rights.
Pass `-Product ckgit -DocsBinary <verified-ckdocs.exe>` for the client installer;
it bundles both executables. The portable ZIP also contains both tools.

The reusable GitHub Windows workflow builds the ZIP and both installers, runs native
tests and checks both installer scopes. Its callers include push CI and the
release workflow. Hosted runners use their supplied `RUNNER_TEMP`; the `V:`
policy above applies to the development workstation. Installers are currently
unsigned.

### AppContainer qualification

`ckgit-lpac-probe` explicitly exercises the sandbox with staged compiler and
CMake binaries. It is separate from CTest because it creates Windows-managed
AppContainer profiles in the user's profile directory. Job data, tools and
temporary output stay beneath the explicit test directory on `V:`.

The current backend requires an administrator broker token to create a named,
private Window Station. Job processes still run in LPAC, with their own
invisible desktop, restricted file access and no network capability. There is
no fallback to the user's interactive desktop or unsandboxed execution.
This desktop supports initialization of Windows libraries used by console
tools; it does not launch Explorer or display a graphical session.

The qualification covers compiler/linker execution, a minimal native program,
a CMake/NMake build and isolation checks. It does not establish compatibility
with every SDK, build system or installer tool, and is not yet a connected
production runner. Non-admin ckdocs installation is independent of this
broker requirement.

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
