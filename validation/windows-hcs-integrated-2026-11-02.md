# Windows HCS runner integration validation — 2 November 2026

The merged implementation is `f5c5a05e84fc4ad04ceb82ccb948e54eae5bcff5`:
permanent source at `6a94b1a` plus the qualified Windows HCS branch at
`44775fd`. It preserves the macOS runner configuration and protocol alongside
the Windows HCS backend. The docs-only follow-up `0efdf08` describes its
operator-prepared status.

The dedicated Windows HCS runner completed hosted Orthomap CI child
`00001793654055817935-2210012b` in 1,161 seconds. Its reported isolation was
`hyperv-hcs`, network denied, 8 GiB memory and a 7,200-second timeout. The
stored 203,754,496-byte tar artifact had SHA-256
`2224b88d8d894e0fa5b6a8210fc2acb62e6442b34a210749faace7c4b38733f6`.
The artifact was downloaded through the ordinary CLI and independently verified:
its ZIP SHA-256 was `f83dbce239a090e24469b92e9c34489daccc768f1714b0735a1adb70dc53ecb0`.
All 18 tests passed; the archive contained 943 files and 18 demo photos. Nine
source checkpoints matched Git commit `fa92e665`, with canonicalization of
Windows CRLF, and the build performed 69 fresh core actions plus WinUI
compilation and linking. After the job, the exact HCS VM
`f0a6744f-2245-49d0-a955-a3de89f78ee9` was confirmed absent without opening an
owning VM handle, and its job workspace was empty.

Combined-source builds and tests were isolated from the running services:

- macOS arm64 Release build, 21:39:37–21:41:51 UTC: process, unit and macOS VM
  result-relay tests passed (3/3; 68.70 seconds). The process-test executable
  SHA-256 was `df932daf7fdb2c221ce06712668bd2eeec002d4267ef20532affad0ac5a599a2`.
- Windows x64 MSVC Release build, 21:40:13–21:42:27 UTC: process, VM channel,
  VM relay and VM configuration tests passed (4/4; 46.02 seconds). The runner
  executable SHA-256 was
  `4972649c3eda284c0e419ec3ab68c9d9311992c9abfc73eae4d2371aa59a552`.
  Its source archive SHA-256 was
  `5c5a47457cc4f077b4bd4a7ea71ed8b0f093ba94294e7abeea4543e396485616`.
- The relay regression includes a 31-second simulated host upload and verifies
  that the guest finishes output and terminal acknowledgement promptly. It
  also checks Pages, prior artifacts when a later job fails, upload failure,
  cancellation, lease loss, malformed guest frames and staging cleanup.

The live R25 runner and server were not replaced by these integration builds.
The standard Windows installer does not yet provision an HCS guest image or
install its guest/preflight tools; see `design/windows-vm-ci.md`.
