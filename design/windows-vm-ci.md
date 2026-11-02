# Windows VM CI backend investigation

Status: a lifecycle primitive is qualified. No Windows VM runner backend is
implemented, registered or advertised. No hosted Orthomap artifact exists.

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

## Backend still to implement and qualify

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
