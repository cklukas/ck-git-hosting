# Proposal: declarative, operator-approved CI toolchains

Status: **work in progress; not safe to use as a CI authorization system**.
The local prototype can parse exact locks and keep CLI review decisions, but it
does not create requests from CI commits, enforce decisions at dispatch or job
start, provision packages, or verify a runner inventory. The current v2 CI
parser rejects new fields. Existing runner labels only select runners, and
Windows jobs copy one operator-configured toolchain tree per job. Nothing here
changes the current CI isolation or grants package managers network access.

The store now provides an internal review transaction that holds the same
request lock as CLI decisions, checks the exact recorded commit and target,
and excludes concurrent revocation while its callback runs. It is not wired
to dispatch and does not verify provisioned bytes or authorize a job by itself.
An additional helper verifies the exact commit's lock and declared source-input
hashes before entering that transaction; it has the same admission limitation.
The gateway's existing hello reports `dependency_admission=0`; this is an
explicit lack of support, not a successful capability negotiation. Internal
queue records can preserve a required lock digest in schema version 6, but the
scheduler holds all such records for every runner protocol until start permits
and inventory verification are implemented. Existing report-free jobs remain
schema 5. No workflow syntax currently creates schema-6 records. The lock digest
identifies the reviewed dependency declaration; a future output inventory
digest is a distinct identity and must not be substituted for it.

## Goals

Projects should be able to request reproducible, platform-specific build inputs
without installing tools on the host or embedding machine paths in workflows.
Operators must decide which inputs are approved, where they are provisioned,
and which projects may use them. A job must see the exact approved dependency
closure read-only and retain the runner's normal isolation and network policy.
Multiple jobs should be able to reuse a large SDK without making a full copy for
each job, once that sharing has been proven safe on the relevant platform.

## Proposed interface

A future workflow schema may specify, per job, a logical toolchain profile and
an exact SHA-256 digest for an immutable resolved lock. This is illustrative,
not syntax accepted by the current release:

```yaml
# PROPOSAL ONLY
version: 3
jobs:
  - name: windows
    runs_on: [windows, x64]
    toolchain:
      profile: orthomap-winui
      lock_sha256: <64 lowercase hexadecimal digits>
    steps:
      - run: [cmake.exe, --build, build]
```

The lock binds a schema version, target OS and architecture, compiler and SDK
identities, build triplet and ABI, exact dependency graph, source or binary
hashes, recipe/registry revisions, required environment and path exports, and
any platform-specific inputs affecting ABI. The lock must also bind hashes of
project dependency inputs such as `vcpkg.json`, configuration and NuGet lock
files; a changed manifest cannot silently reuse an old resolution. Identifiers
are data: workflow inputs never specify host paths, shell commands, elevated
hooks, arbitrary download URLs, or credentials.

## Request and approval experience

The project requests dependencies; it does not install or approve them. Resolve
each request to an exact, reviewable set of direct **and transitive** packages,
with package identity, version, source, content hash, platform, ABI and the
complete lock digest. Show which packages are already approved for that
project and target, which are new or changed, and whether provisioning requires
running package recipes. Dependency names and descriptions are untrusted input.

An authorized administrator can approve or reject individual exact entries,
or explicitly approve the displayed complete set. A set is ready only when
every required entry and the exact resolved lock are approved for the project,
platform and provisioning policy. An approval for one version, hash, source,
target or recipe does not approve another; changed inputs require a fresh
review. A partial approval never starts a job with missing inputs. Record who
approved or rejected each request, when, and the precise identity and scope.
Support revocation and a clear policy for already-running jobs and cached
bundles; revocation must block future use without deleting data under active
jobs. Request, approval, provisioning and job-start checks must be atomic with
respect to a changing lock or revoked approval, so reviewing one request can
never authorize a different one.

All approval, rejection and revocation operations are CLI-only, through the
existing `ckgit-admin` server-side administrative interface, alongside its
`ci` and `runner` commands. The local prototype does not yet enforce those
decisions in CI. The dependency view in the web dashboard is read-only: it
shows requests, exact dependency identities, approval and provisioning status,
and available toolchains, but offers no approval or other mutation endpoint.
CLI actions must apply the same centralized policy checks and write an audit
record. Approval authorizes provisioning only within that policy; it does not
run repository-controlled commands as administrator or grant the CI job network.

The local prototype currently accepts the following commands. Their review
records are **not yet enforced by CI** and must not be used to authorize jobs:

```text
ckgit-admin ci dependencies list PROJECT --config SERVER_CONFIG
ckgit-admin ci dependencies request PROJECT COMMIT LOCKPATH --config SERVER_CONFIG
ckgit-admin ci dependencies check PROJECT COMMIT LOCKPATH --config SERVER_CONFIG
ckgit-admin ci dependencies register PROJECT LOCKFILE --config SERVER_CONFIG
ckgit-admin ci dependencies approve PROJECT LOCK_SHA256 PACKAGE --config SERVER_CONFIG
ckgit-admin ci dependencies reject PROJECT LOCK_SHA256 PACKAGE --config SERVER_CONFIG
ckgit-admin ci dependencies revoke PROJECT LOCK_SHA256 PACKAGE --config SERVER_CONFIG
```

`request` reads the lock and each declared input from an exact Git commit and
checks the declared input hashes before recording the request. `register` is a
manual import without that commit verification. Neither path approves a package,
provisions a toolchain, or causes CI to wait for approval in the prototype.
`check` re-reads the exact commit and its input hashes, then reports whether
that commit's exact lock has complete package review. It exits nonzero if the
commit was not requested or review is incomplete. It is a point-in-time review
snapshot: it neither provisions tools nor authorizes a CI job, and a later
revocation can change the result.

The command must show the exact immutable request and dependency identities
before an approval and reject a request that changed since it was displayed.
Noninteractive approval must name exact immutable IDs, never an implicit
latest request. A separate explicit action can approve the entire displayed
closure. Dashboard pages can be viewed wherever the existing read-only
dashboard is exposed, so they must not disclose provisioning credentials or
private registry secrets.

A profile is an operator-controlled mapping and access policy, not an executable
script from the repository. Exact compatibility, canonical lock encoding,
package-specific verification and update policy must be specified before the
schema is finalized. A versioned protocol/capability negotiation must prevent
older servers or runners from silently ignoring the declaration; parser,
scheduler, runner and status UI must be upgraded together or fail closed.

## Lifecycle and trust boundaries

1. A project declares dependency inputs with their normal package manager
   manifests. Resolution produces a deterministic lock and a reviewable request;
   project code can propose but cannot approve its own privileged installation.
2. An operator approves the resolved digest and project/profile access. A
   separately invoked provisioning service or administrator-controlled process
   retrieves only policy-approved sources and verifies exact hashes, source
   identities and lock completeness before materialization. Credentials are
   confined to provisioning and never exposed to job code.
3. Package recipes may execute code, including vcpkg ports and build scripts.
   Treat all of them as untrusted: execute without host administrator rights in
   a disposable isolated builder with explicitly limited network and filesystem
   access, resource limits, and no runner keys or production toolchain write
   access. Static archive extraction must reject traversal, reparse points and
   unsafe links. Administrator approval of a lock is not permission to execute
   arbitrary repository scripts as administrator.
4. Validate the built result and publish atomically under a content-derived
   identity, with protected ownership and immutable permissions. Concurrent
   provisioning must not expose a partially populated store. Version updates
   create new identities; in-use identities remain stable until jobs finish.
5. The scheduler considers only eligible, online runners whose verified inventory
   and project policy include the exact digest and platform. A missing, stale,
   unauthorized or corrupt profile is reported distinctly from an ordinary busy
   runner; no implicit substitution, privileged installation or network access
   occurs. Runner availability and inventory must be verified at dispatch and at
   job start to handle updates, disconnects and stale advertisements.
6. The runner exposes the exact approved inputs read-only inside the sandbox,
   exports their documented paths, and records the resolved digest and execution
   isolation with the result. Cleanup removes only job-owned state. On Windows,
   LPAC access to a shared store requires a separate, verified ACL/identity and
   concurrent-lifecycle design: the existing implementation stages and seals a
   private copy. Do not replace it with writable shared trees, unverified
   hardlinks, reparse points or broad host ACL changes.

A content-addressed SDK is a trusted provisioned input, not the existing
job-writable build cache. Any reusable compilation cache must be separated by
project/trust domain and all relevant compiler, SDK, lock, target and ABI inputs;
it must not be promoted into an approved SDK merely because a job wrote it.
Validation must include concurrent jobs, cancellation, stale inventory, corrupted
store content, ACL escapes, cache poisoning and rollback while jobs are active.

## Orthomap and platform boundaries

Orthomap's pinned vcpkg baseline and package manifests, pinned Windows App SDK
and CppWinRT dependencies, and separately pinned reconstruction engines are
inputs to a Windows lock, not host installation instructions. Exact versions
alone are insufficient: the full transitive graph, binary/source hashes,
compiler/SDK and triplet must be bound too. An approved NuGet package feed can
be staged for offline restore if the package graph and hashes are fixed.

The macOS GUI needs its own arm64 lock and approved Apple toolchain/SDK and
third-party inputs. The existing POSIX runner has Linux namespace isolation;
its macOS code does not implement a qualified sandbox. The user now prefers a
native Apple sandbox if its process lifetime and security can be qualified;
see [native process supervision](macos-native-process-supervision.md). A
separate macOS virtual machine remains an alternative, not a prerequisite for
native CI. The VM backend under development uses Apple's Virtualization
framework and Apple-provided VM tooling exclusively, with no third-party VM
code or tools. Design that runner around
disposable job VMs or a verified clean reset, and ensure cancellation and job
completion tear down the entire guest, including detached descendants. The
host must retain its credentials and files outside the guest, restrict guest
network access, and expose only the approved inputs and job data. VM creation,
health checks, cleanup, tool access and Apple licensing must be implemented and
qualified before Mac jobs run. Failure to establish or clean up isolation must
fail closed; do not register an unconfined or degraded Mac runner.

The macOS CI setup command must require an operator-selected, absolute VM work
directory and use it for disposable VM disks, snapshots, guest job data and
other VM intermediates. For this installation the preferred location is a
dedicated directory on `/Volumes/PRO-BLADE`; do not silently fall back to the
system disk or another volume if it is unavailable. Keep only the explicitly
configured reusable base image, if the chosen VM technology requires one, and
identify it separately from job-owned files. Each job must use a bounded,
uniquely identified disposable workspace and remove its disk, snapshots and
other intermediates after guest shutdown, including after cancellation or
failure. On startup, reconcile interrupted jobs and clean up only positively
identified, inactive runner-owned workspaces; never remove a base image, an
active VM or unrelated data. If cleanup or storage limits cannot be verified,
stop accepting jobs and report the problem. Provide configurable storage
limits and report available space and cleanup failures through the existing
administrative interface. These remain requirements for the unfinished runner.

The experimental VM backend can claim and run offline jobs. Its admin setup
records base and work paths and validates ownership, permissions and space. Its
APFS clone implementation requires both directories on the same volume and
reserves enough free space for the full logical VM disk plus the configured
reserve. The read-only `ckgit-admin runner macos-vm status` command reports
the configured storage, reserve and work-root entry counts; it cannot prove
that a particular VM is stopped. A recognized workspace from an earlier host boot can be removed; one
from the current boot is recovered only with a durable helper stop confirmation
and its exclusive lock, and otherwise blocks another clone. The
runner uses one disposable clone at a time and recovers marked host relay data
only after VM preflight. Real helper-crash and host-crash qualification remain
incomplete; see [macOS runner development](../docs/operations/runner-macos-development.md).

The guest/host job channel uses Apple's Virtio socket device and the macOS
guest's `AF_VSOCK` interface. The host-side broker must retain the
runner SSH key and lease token; neither belongs in the guest. It must limit
every checkout and output to the claimed job, validate framing, paths and byte
counts, and enforce the server's artifact/log limits. Guest input and results
are untrusted even when the VM was started from a clean base. Offline real-VM
tests cover the channel, source staging, log and artifact relay, cancellation,
timeout and lease loss. The helper exposes a read-only input share and CI VMs
have no virtual network device. These tests do not complete crash or toolchain
qualification, and the channel does not enforce dependency approval.

Guest tool paths and environment values are currently configured manually by
an operator for tools already installed in the base. They are not a verified
inventory or an approved dependency profile. The VM runner must not be
advertised as satisfying a project dependency lock until the approval,
provisioning and job-start enforcement described above are implemented and
tested. In particular, the separately verified Orthomap OpenMVS archive has
not been installed or approved merely because it is available locally.

An operator may optionally configure a bounded pool for later reuse, including
limits on retained images, disk consumption and age. The default is no retained
job images. Reuse must start from a verified clean base or a verified clean
snapshot; a previous job's modified disk, processes, credentials or output
must not carry over, even for a later commit on the same branch. If project- or
branch-scoped pools are supported, scope is an additional restriction and does
not replace that reset. Eviction removes only inactive, positively identified
runner-owned images. Reconciliation after a crash must not consider an image
reusable until the clean state and the VM's termination are verified. Exceeding
the configured bounds or being unable to verify cleanup must prevent new jobs
rather than accumulating images or using another volume.

An exploratory macOS test demonstrated denial of a selected test file read,
file write and network connection. A trivial Swift program compiled when its
module cache was explicitly placed in the job directory, and a C object compiled
when its log was also placed there. That experimental policy allowed broad
filesystem reads and Mach access; it is not a safe CI policy. `sandbox-exec` is deprecated on the tested
macOS version. A sandboxed child also successfully called `setsid()`, escaping
the process group used by the existing cleanup mechanism; this test did not
show an escape from the sandbox itself. A separate local probe forked, called
`setsid()`, executed another copy of itself and was still denied access to a
test file outside its allowed directory. This is evidence for that particular
file check, not a general security audit. No platform isolation guarantee or
runner readiness follows from these probes.

Paglets' macOS worker uses a `sandbox_init` Seatbelt policy that denies fork,
exec, network access, file writes and file-content reads after the worker is
initialized. Its Wasm worker therefore does not need a build-style process
tree. A native CI sandbox would need a different policy that allows the
compiler chain and the necessary workspace and SDK access. It could avoid a VM
if a separately qualified mechanism also reliably terminates all job-owned
processes (including detached descendants), keeps credentials and other host
data inaccessible, and enforces the network and filesystem restrictions. A
dedicated job identity and supervisor is one possible direction, not yet an
implemented or verified guarantee. Apple's `sandbox_init` interface is
deprecated; compatibility and maintenance of any native policy need explicit
evaluation.

An updated diagnostic used an isolated temporary snapshot of Orthomap commit
`5e6b0e8488dc17d131fa0794d8f21b3603ed16e8` on macOS 27.2. Its C++/SwiftUI
Release build completed under a deny-default Seatbelt policy, and all 19
CTests passed with a clean environment after allowing signals to direct
children. A separate probe confirmed that the same signal rule denied a signal
to a task-owned unrelated process. The policy allowed read access to broad
Homebrew and Apple toolchain paths and used Swift's `-disable-sandbox` to avoid
a nested compiler sandbox; the outer policy remained in place. These tests do
not establish process-tree cleanup, safe access to every allowed toolchain
path, runner-secret protection, or a production security boundary.

## Incremental delivery

- Specify the lock format, operator approval policy and scheduler/runner protocol;
  agree the contract with the first consuming project before advertising it.
- Implement strict parsing, negotiation, inventory, placement and actionable
  status, preserving existing workflows and failing closed on unsupported jobs.
- Implement a bounded provisioning pipeline and reproducibility/integrity tests;
  begin with operator-created immutable bundles before adding package resolvers.
- Implement and qualify efficient platform-specific read-only sharing; until
  then, retain the existing private-copy Windows path and its security checks.
- Add a qualified macOS VM-backed runner independently. Validate real
  Orthomap GUI builds and artifacts on both platforms before calling the
  declarative workflow supported.

Until these pieces exist, operators can assign specific `runs_on` labels to a
manually prepared runner. Labels only route jobs: they do not verify versions,
resolve dependencies or prove that a toolchain is immutable or safe.
