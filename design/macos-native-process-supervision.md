# Native macOS job lifetime investigation

2026-11-02. The user prefers native CI to reduce Mac resource usage and
operational complexity. The peer owning the platform investigation agrees that
an exclusive unprivileged UID per concurrent job slot, with an independent
supervisor, is the preferred next native qualification path. Detached processes
do not by themselves require a VM. Neither native nor VM hosted Mac CI is
qualified for enabling jobs yet.

## Implementation follow-up

The native supervisor, authenticated client, runner integration and admin
setup are now implemented. See [native setup and qualification](../docs/operations/runner-macos-native.md).
The root-only integration suite exercises actual account and launchd lifecycle;
its execution status must be checked separately from the passing unprivileged
policy test. Setup leaves the hosted broker stopped until qualification succeeds
and the operator starts it explicitly.

## What we can identify

The existing `signalProcessTree` in `src/common/process.cpp` signals the
original process group and, when alive, its leader. A descendant can change
groups or sessions. If its intermediate parent exits, the descendant can be
reparented to PID 1: a later parent-PID scan has no ancestry left to recover.
Polling faster cannot guarantee observing a short-lived intermediate parent.

The installed macOS 27.2 SDK and [Apple's event.h](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/event.h)
mark `NOTE_TRACK`, `NOTE_TRACKERR` and `NOTE_CHILD` unsupported since 10.5.
`NOTE_FORK` reports that the watched process forked, but does not deliver the
child PID. It does not supply a recursively inherited membership handle.
[Apple's launchd manual](https://github.com/apple-oss-distributions/launchd/blob/main/man/launchd.plist.5)
limits automatic cleanup to the job's original process group; leaving
`AbandonProcessGroup` false does not solve detachment.

UID membership is different: ordinary fork, exec, setsid and reparenting do
not discard it. `proc_listpids(PROC_UID_ONLY, uid, ...)` can discover these
processes without reconstructing ancestry. The UID must belong exclusively
to the active job. Sweeping the developer's login UID would include unrelated
applications and is unacceptable. A shared build UID also cannot distinguish
concurrent jobs.

## Reproduced evidence

[The diagnostic](../tests/platform/macos_descendant_probe.py) passed 25/25
trials on macOS 27.2 (26B123), with results and its source SHA-256 in
[the validation record](../validation/macos-descendants-2026-11-02.json).
Each trial creates a job root, forks an intermediate that calls `setsid`,
forks again, and execs a fresh Python interpreter. The intermediate exits.

In every trial, the surviving descendant had PPID 1 and a different session
and process group from the job root. The root's child list was empty. Killing
the root's original process group left the descendant alive. UID enumeration
found it before and after that cancellation. The root's fork notification
contained the root PID and data 0, not a descendant PID.

The diagnostic used the current unprivileged UID only for discovery, never
for broad signaling. It killed only its own unreaped root's process group.
Each orphan exited on a private control pipe closing, and its exit was
observed with kqueue; child alarms bounded failure paths. The pipe is a test
fixture, not a proposed supervisor: arbitrary jobs can close inherited FDs.
This experiment was unsandboxed and did not create accounts, test credential
transitions, or qualify production cleanup. The peer's separate Seatbelt
Orthomap build and 19 passing CTests remain complementary evidence.

## Proposed native implementation

Start with one exclusive build identity and one concurrent job. A bounded
pool of identities can support concurrency later. These are non-login,
unprivileged identities, never used for interactive work or other services.
Reserve and record the identity before starting any untrusted code.

Keep credentials, lease ownership, cleanup state and the supervisor outside
the job identity and sandbox. A narrowly scoped privileged launcher sets all
credentials and supplementary groups, closes inherited capabilities, installs
the job's Seatbelt policy and resource limits, then executes the job. The job
must not regain privilege, change identity, or delegate persistent execution
to launchd/XPC or another service outside the controlled identity. These
restrictions need actual tests with the production policy and toolchain.

At completion, cancellation, timeout or runner loss, stop admission to the
identity; enumerate all matching processes, stop/terminate them, and rescan
until verified empty. Forking during enumeration means one sweep is not
enough. Bound cleanup time and quarantine the slot on uncertainty or failure.
Do not report clean completion, delete its workspace, or reuse its UID until
cleanup succeeds. Independent watchdog/restart reconciliation must recover
the durable ownership record and clean interrupted jobs before accepting work.

PID reuse must not let a privileged supervisor signal another user's process.
Re-reading a PID's start time alone still leaves a check-to-signal race.
Qualify a signaling mechanism constrained by the job's credentials (or an
equivalent kernel-enforced target identity), rather than root signaling a
cached PID list. The trusted watchdog must survive hostile job signals.
Process limits and bounded enumeration/cleanup are required against fork
storms; UID membership alone is not a resource limit or filesystem sandbox.

## Qualification gates before enabling native CI

- Detached sessions, changed process groups, double forks, rapid parent exit,
  exec chains, closed output FDs and a successful root exiting with live workers.
- TERM-ignoring and stopped workers, forks during cleanup, bounded fork storms,
  PID churn/reuse and another user's sentinel remaining alive.
- Failed identity transitions, set-ID execution and service-mediated launches;
  concurrent job separation and no stale processes across identity reuse.
- Supervisor and broker crashes, forced runner termination and startup recovery;
  failed enumeration/signaling must quarantine rather than silently succeed.
- Real compiler and test workloads under the final filesystem, network, signal,
  credential and resource policy, including the existing Orthomap scenario.

This investigation establishes why ancestry polling is insufficient and why
exclusive UID membership is a plausible native solution. It does not claim
the proposed cleanup algorithm or full isolation boundary has passed these gates.

## Empty-snapshot correctness

The implementation does not filter vanished or zombie PIDs into an apparent
empty result. It requires an empty raw UID enumeration, reaping its direct
child and retrying stale entries within the cleanup deadline. In
[Apple's proc_listpids implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/proc_info.c),
UID matching and PID collection occur while holding `proc_list_lock`. Identity
changes and outside service launches must remain forbidden, and the supervisor
must remain the only source of new launches for this reserved identity. A
rapid parent/child handoff case is included in the privileged qualification.

## Other mechanisms considered

[Endpoint Security](https://developer.apple.com/documentation/endpointsecurity)
requires its client entitlement. Its [sequence-number documentation](https://developer.apple.com/documentation/endpointsecurity/es_message_t/seq_num)
explicitly accounts for dropped events. An event-derived lineage tracker needs
recovery and cannot simply assume every fork notification arrives.

Apple's [coalition implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_coalition.c)
also warrants care: requesting coalition termination is not a kill-all operation;
existing members can still fork. No supported, qualified coalition supervisor
was established here. It is not a reason to prefer that complexity over the
exclusive-identity prototype.
