---
description: "Native macOS CI runner setup and qualification with an exclusive build identity."
---
# Native macOS runner

The native backend uses Apple's Seatbelt sandbox and an exclusive non-login
build account. A root launchd supervisor owns cleanup; the SSH broker runs as
a different, unprivileged account. It requires no VM. The initial implementation
supports one active job per installation, offline execution, and ephemeral
caches. Each step ends with a process drain; background processes do not survive
a step boundary. It does not permit degraded execution.

The setup command **does not start the SSH broker or claim hosted jobs**. It
installs and tests the supervisor, then leaves the broker's launchd file ready
for an explicit start. A runner cannot claim work unless the supervisor has a
successful qualification record matching its executable, configuration and
macOS build. A failed cleanup or qualification prevents admission.

## Prepare the broker

Use an existing unprivileged service account, its private SSH identity, a
verified `known_hosts` file, and a runner configuration containing `server`,
`name`, `ci_build_root`, `identity` and `known_hosts`. Admit its public key on
the hosting server through the existing `ckgit-admin runner add` workflow.
Do not use root as the broker, and do not use the generated build account as
an SSH account. Prefer a dedicated service account over an interactive login.

Build `ckgit-admin`, `ck-ci-runnerd` and `ckgit-macos-native` together. They
must be in the same directory for setup. Choose an existing, traversable
parent directory beneath a mounted `/Volumes/S8T/tmp` (preferred) or
`/Volumes/PRO-BLADE/tmp`. The job work directory must be new or empty. A missing
volume is an error; setup never falls back to the system disk. Keep the SSH
key and broker home outside the job work directory.

## Preview and install

With paths and account names adjusted to the installation:

```sh
export TMPDIR=/Volumes/S8T/tmp/native-setup-task
mkdir -p "$TMPDIR"
ckgit-admin runner macos-native setup \
  --config /absolute/path/to/runner.ini \
  --broker-user ckgitrunner \
  --work-root /Volumes/S8T/tmp/ckgit-native-jobs \
  --toolchain /Applications/Xcode.app/Contents \
  --dry-run

sudo ckgit-admin runner macos-native setup \
  --config /absolute/path/to/runner.ini \
  --broker-user ckgitrunner \
  --work-root /Volumes/S8T/tmp/ckgit-native-jobs \
  --toolchain /Applications/Xcode.app/Contents
```

Verify the selected volume is mounted before creating the task directory.
`--toolchain` is repeatable. Each path grants read and executable access to an
operator-approved directory under `/Applications`, `/Library/Developer`, or
`/opt`; do not select a tree containing credentials. No toolchain is downloaded
or installed by this command. Compiler caches and temporary files are forced
into the active workspace. Existing `bin`/`usr/bin` directories in approved roots
(and the standard Xcode compiler directory) are prepended to the step PATH.
A selected Xcode root also pins `DEVELOPER_DIR`; the policy allows its specific
system license-preference file, without permitting general preference access.
Toolchains remain operator-managed trusted inputs;
requalify after changes. Swift builds may need the compiler's
`-disable-sandbox` option for its nested driver sandbox; the outer job sandbox
must remain enabled. For that integration, the trusted launcher sets
`CKGIT_MACOS_SANDBOX=seatbelt-v1` only after Seatbelt installation and only when
the root-owned qualification matches the helper/configuration/macOS build. It
removes any workflow-supplied value first. Consumers may use this exact marker
inside runner jobs for nested Swift sandbox compatibility; `CKGIT_CI` alone
is insufficient. Like any environment variable, it is not independent proof
of confinement when manually set outside this runner.

Setup performs these actions:

1. Validates the broker, private key, pinned-host file, scratch volume, empty
   workspace and explicit toolchain paths.
2. Allocates an unused UID/GID in 61000–61999 for `_ckgit_<runner-name>`, with
   disabled password, `/usr/bin/false` shell and `/var/empty` home. It refuses
   to adopt an existing account or overwrite an existing installation.
3. Installs root-owned helper/runner executables under
   `/Library/PrivilegedHelperTools/org.ckgit.native.<name>/` and root-controlled
   state under `/Library/Application Support/ckgit-native/<name>/`.
4. Saves the original configuration as private rollback evidence and writes a
   separate installed `runner.ini`, selecting the authenticated native socket,
   offline execution and the selected work root. The input configuration is
   not overwritten. A prior optional VM configuration is cleared in this copy.
5. Installs and starts `org.ckgit.native.<name>` as a root launchd service.
   Installs, but does not start, `org.ckgit.runner.<name>` under the broker account.
6. Runs restriction, detached-process, fork-burst, rapid parent/child handoff, cancellation, closed-output
   timeout, disconnect, and supervisor-SIGKILL/restart checks. A failure leaves
   the broker disabled and prints the failing check.

On success, setup prints the installed runner configuration and the exact
`launchctl bootstrap system ...` command to start the broker. Use that installed
configuration, not the original input. The native socket is accessible only to
its broker UID, and the client verifies that the server is root. A broker flock
prevents concurrent runner instances from sharing the build root.

## Cleanup and recovery

The supervisor records the active workspace before transferring ownership or
launching untrusted code. The child drops real/effective/saved credentials and
supplementary groups, closes inherited descriptors, installs limits and
Seatbelt, and executes in the workspace. Set-ID executables in allowed tool
paths are denied. Network and Mach-service access are denied by default.

Completion, cancellation, timeout and broker disconnection all trigger bounded
UID enumeration, stop/kill sweeps and verification. Completion requires an empty
raw UID snapshot; stale PIDs and zombies are retried, never filtered into a
false empty result. The direct child is reaped during cleanup. Signals are delivered by an
expendable helper running with the job UID, so a recycled PID cannot authorize
signaling another user's process. The root supervisor survives job signals.
Only after the UID is empty does it return workspace ownership to the broker
and report completion. On restart, launchd starts the supervisor and it drains
the UID and reconciles the active journal before opening its socket. Even if
the work volume is missing, UID cleanup precedes workspace validation.

Limits include 256 processes, 512 descriptors per process, a 16 GiB file-size
limit, and a sampled 16 GiB aggregate resident-memory ceiling. Cleanup has a
10-second deadline; failure quarantines the slot rather than accepting another
job. Files with multiple hard links and special filesystem objects cause
workspace validation to fail closed. Artifact operands, the checkout root,
Pages roots and artifact staging directories cannot traverse symlinks; the
broker must not be redirected into its private host files during collection. This may reject build tools that depend
on those objects; no unsafe ownership recovery is attempted.

For manual recovery, first stop the broker and supervisor through their exact
launchd labels. Then run the installed helper's root-only
`reconcile '/Library/Application Support/ckgit-native/<name>'` command. It takes
the supervisor lock, verifies the account, drains its processes, and restores
the recorded workspace. Do not delete the build account or journal before
reconciliation succeeds. Do not rerun setup over a partial installation.

## Development validation

The unprivileged CTest `macos-native-policy` exercises the actual generated
policy but does not prove exclusive-UID cleanup. The privileged integration
script tests the admin route, real account/service installation, and recovery:

```sh
sudo /bin/sh tests/integration/macos_native.sh \
  /absolute/path/to/build/bin existing-unprivileged-user \
  /Volumes/S8T/tmp/unique-native-qualification-task
```

It creates an isolated test installation, never starts its SSH broker, and
removes the test account/services after successful reconciliation. Failed
recovery preserves that isolated state for inspection. Do not treat an
unprivileged policy test as a substitute for this qualification.
