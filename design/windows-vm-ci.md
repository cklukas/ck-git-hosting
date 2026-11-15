# Windows VM CI backend investigation

Status (2 November 2026): a dedicated offline HCS runner completed a hosted
Orthomap Windows build and produced an artifact. Native Windows channel,
configuration, result-relay and process tests passed, including a 31-second
output upload and VM cancellation/owner-death qualification. The standard Windows runner installer includes the guest, preflight utility and
operator-run image preparation scripts. Image preparation remains an explicit
administrator action; see the Windows runner setup guide.

## Why another Windows boundary is needed

The current production Windows executor uses LPAC and denies network access.
Orthomap's native core tests and native SDK metadata generation can execute
there. The WinUI 1.8 XAML compiler cannot finish: schema loading calls
`System.Reflection.Adds.MetadataDispenser.GetDispenserShim`, then Framework
`RuntimeEnvironment.GetRuntimeInterfaceAsObject` / `Marshal.GetObjectForIUnknown`,
which fails with E_ACCESSDENIED in LPAC. The SDK's unmodified net6 compiler has
the same call; a focused invocation on the staged .NET 9 runtime throws
PlatformNotSupportedException outside LPAC too. Switching task runtimes cannot
remove that dependency. SDK binaries, COM permissions and the isolation policy
have not been changed.

A VM executor is a separate backend, preserving the existing LPAC default.
There must be no `allow_degraded` escape path or job-controlled isolation marker.

## Qualified host lifetime primitive

Microsoft's HCS `HcsCreateComputeSystem` supports a full VM with
`ShouldTerminateOnLastHandleClosed=true`. The host queries that policy after
startup; it does not rely on configuration text alone. Our opt-in probe starts
unique empty VMs with no disks, NICs, shared folders or guest OS. It confirms
removal after normal handle closure, then starts a separate owner process,
forcibly terminates that exact process handle, and confirms removal of its VM.
All waits are bounded. It never opens or controls a pre-existing VM.

The 2 November 2026 workstation run passed both cases. See
`validation/windows-vm-lifetime-2026-11-02.json`. This does not qualify a booted
Windows guest, job execution, clean-image reset, transport or hosted results.

The standalone source/build entry is
`tests/platform/windows-vm-lifetime/CMakeLists.txt`; it is deliberately outside
the ordinary test suite because it creates VMs and requires an administrator or
Hyper-V administrator. On the workstation, select an existing private TMPDIR on
V: and set TEMP/TMP/TMPDIR before configuring, building and running CTest. The
probe enforces that qualification location. Its source and build records must
be verified before execution. Do not reuse the GUI VM or recovery VM.

## Implemented topology

- An operator-selected, sealed Windows base without host credentials; all OS,
  tool and development disk paths configurable. Workstation paths stay on V:.
  Do not mutate or clone a shared live user/GUI VM.
- A fresh differencing OS disk and fresh writable development disk per job;
  immutable SDK inputs on a read-only virtual disk. No host folder shares.
  Guest sources, caches, test data, output and TEMP remain on guest V:.
- A full HCS VM with transient state, no NIC, no redirected clipboard, audio,
  printers, user devices or host credentials. CPU/memory/time/storage budgets
  must be operator controlled. Clean reset is required before every job.
- A Hyper-V socket channel bound to the exact new VM ID. The host owns the
  lease and SSH identity. Reuse the bounded VM assignment/result protocol where
  possible; never pass credentials, usable lease tokens or guest-selected host
  destination paths. Every returned frame is untrusted and budget checked.
- An explicit guest executor: it may build outside LPAC only within the outer
  VM boundary. This must not become a public unsandboxed host execution mode.
  Runtime/compiler telemetry opt-outs are forced by the executor.
- The broker owns a non-inherited HCS handle. Cancellation, disconnection,
  forced helper/owner termination and host reboot must leave no running job.
  Confirm VM exit before committing a terminal result or deleting its disks.
  Uncertain state quarantines the workspace and stops further claims.
- Startup recovery must identify only this runner's inactive work using
  ownership records and exact IDs; it must never sweep other tasks' files/VMs.
- Qualify a real offline WinUI build, descendant escape attempts, normal/error
  teardown, forced owner death, clean subsequent job and artifact return. Then
  run the real gateway/server path and download/verify the app artifact.

## Primary API references

- [HCS schema](https://learn.microsoft.com/en-us/virtualization/api/hcs/schemareference)
- [HCS operation sample](https://learn.microsoft.com/en-us/virtualization/api/hcs/reference/operationcompletionsample)
- [CLR metadata API limitations](https://learn.microsoft.com/en-us/dotnet/api/system.runtime.interopservices.runtimeenvironment.getruntimeinterfaceasobject)

## Operator selection and admission

`ci_windows_backend=hyperv-hcs` selects this backend explicitly. LPAC remains
unchanged by default. HCS requires `ci_allow_network=false`, no shared cache,
and an absolute `ci_windows_vm_profile`. The profile has exactly these fields:
`schema_version=1`, `os_disk`, `os_sha256`, `work_disk`, `work_sha256`,
`sdk_disk`, `sdk_sha256`, `memory_mb` and `processors`. All image and temporary
paths are on V:. Images are hashed with deny-write/delete handles retained for
the broker lifetime. The operator supplies already initialized images; jobs
cannot install software, select a base or provide arbitrary HCS JSON.

A job receives fresh native differencing disks and a read-only SDK disk. Its
GUID ownership record is flushed before HCS creation. Recovery checks only
those exact recorded IDs, refuses unknown/reparse entries and requires absence
before deleting a workspace. Unconfirmed stop or cleanup is fatal to claiming.
Host SSH credentials and lease tokens never enter the guest. Returned terminal
success is provisional until shutdown and workspace cleanup are confirmed.
The host replaces guest execution metadata with `windows/x64`, `hyperv-hcs`,
network denied and its configured resource limits.

`ckgit-windows-vm-host-check --preflight` creates a diskless disposable VM.
`--storage-check PROFILE WORKROOT` verifies image hashes, locks, differencing
creation and absence-checked cleanup without booting an OS. The operator-only
`--initialize-base OS WORK SDK ACCEPTANCE` boots only explicitly selected new
base images, allows bounded Windows setup restarts, waits for the guest's
setup-complete readiness greeting, and requires clean shutdown before success.
A matching human license acceptance record is mandatory. The guest also
requires the machine Visual Studio `OptIn=0` policy before it can accept a job.
No project code or host credential is sent during initialization.

These local checks do not by themselves qualify a runner: validate the actual
workload, service account, result download and recovery on the target machine.

## Boot and diagnostic progress (2 November 2026)

The dedicated Hyper-V socket application is registered by the explicit operator
script `scripts/register-windows-vm-socket.ps1`; the broker checks that exact
service GUID and ownership before claims. Socket descriptors use file rights
and grant host connections only to the owning broker SID. Guest binding uses
`HV_GUID_PARENT`. Readiness checks report bounded pre-job errors over this
channel instead of silently failing before the host can connect.

The R16 offline initialization reached the actual guest service and returned
`cannot mount guest disk role`; no source, job or credential was sent. R17 adds
the exact mount path and Win32 code, host-owned result timestamps, synthetic
console diagnostics and explicit integration-service shutdown/time sync. The
MSVC build compiled four changed C++ inputs plus the operator qualification
fixture, and all three channel/relay/config tests passed. R17 actual guest
readiness and clean shutdown are still unqualified.

Only initialization may retain a failed pre-job development disk for trusted
operator diagnostics after VM absence is confirmed. A subsequent initialization
recovers that exact owned workspace. Ordinary job disks are never mounted by
the host after execution and are removed only after confirmed VM absence.

`ckgit-windows-hcs-self-build` is an explicit operator-only full-workflow fixture;
it is not registered as a CTest and its existence is not full-job validation.
