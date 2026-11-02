---
description: "Development status of the Apple Virtualization macOS CI runner."
---
# macOS runner development

The macOS VM backend is experimental. A configured remote runner can claim and
execute jobs in a disposable Apple VM. A synthetic offline job, artifact and
protocol-2 gateway tests have passed, including an isolated gateway on rpi4
accessed over SSH using an existing account and a command wrapper. A separate
isolated SSH service on rpi4 also passed a real VM job using the generated
restricted runner key and forced command; the production server's own runner
registration was not changed. An Xcode build, forced-helper termination and
host-crash recovery have not yet been qualified. Use it for development
qualification until those checks pass.

After Xcode first-launch setup, an offline disposable VM job compiled and ran a
small C program with Xcode's Apple Clang toolchain and returned the executable
as an artifact. This verifies a minimal toolchain path, not an `xcodebuild`
application build or the remaining crash-recovery gates.

Opt-in real-VM cancellation and timeout tests also passed: the host received
the expected terminal result, stopped the VM, and removed its disposable clone.
This includes cancellation during guest boot, before the guest handshake.
A simulated lost lease during a real VM job also stopped the VM and removed
its clone without publishing a terminal result. An isolated gateway test also
revoked a real lease during a VM job; the runner stopped the VM and cleared its
relay data. A brief interrupted SSH stream also reconnected and completed a VM
job through an isolated network gateway. In a separate isolated test, an
outage longer than the lease caused the gateway to requeue the job and the
original runner to stop its VM. These tests do not establish recovery after a
helper or host crash.

A test killed a surrogate runner after the real guest connected; the surviving
helper confirmed shutdown and the workspace was recovered without a reboot.
This does not qualify forced termination of the helper or a host crash.

A real offline job also verified staged input, a read-only exchange, no
inherited host Virtio socket, and no active non-loopback IP interface. A
specific host-only test file was inaccessible from the guest. These are focused
checks, not a complete isolation audit.

The separate Apple Seatbelt-based runner is documented in
[macOS native runner](runner-macos-native.md). Its qualification status and
setup are independent of this experimental VM backend.

On the Mac, build and install the runner component together. For example,
choose an existing build directory and a private installation prefix:

```sh
cmake --build /absolute/path/to/build --target ckgit-admin
cmake --install /absolute/path/to/build --component macos-vm-runner \
  --prefix /absolute/path/to/ckgit-vm-runner
```

The component installs `ckgit-admin`, `ck-ci-runnerd`, `ckgit-macos-vm` and
`ckgit-macos-vm-guest` into the prefix's `bin` directory, and the guest setup
script under `share/ck-git-hosting/macos`. Add the `bin` directory to the
operator's PATH. The VM helper uses Apple's Virtualization framework and is
signed with the virtualization entitlement. Keep the admin, runner and VM
helper executables together. The guest executable is installed inside the
base as described below. Installing this component does not register or start
a runner.

Prepare an Apple macOS restore image and a dedicated APFS work volume. For
example, with all paths adjusted to existing directories on that volume:

```sh
ckgit-admin runner macos-vm restore-info
# Download the returned Apple URL to /Volumes/PRO-BLADE/tmp/ci-setup/restore.ipsw.
ckgit-admin runner macos-vm install /Volumes/PRO-BLADE/tmp/ci-setup/restore.ipsw \
  /Volumes/PRO-BLADE/tmp/ci-setup/base 128
ckgit-admin runner macos-vm setup /Volumes/PRO-BLADE/tmp/ci-setup/base
mkdir -m 700 /Volumes/PRO-BLADE/tmp/ci-setup/jobs
ckgit-admin runner macos-vm configure --config /path/to/runner.ini \
  --base /Volumes/PRO-BLADE/tmp/ci-setup/base \
  --work-root /Volumes/PRO-BLADE/tmp/ci-setup/jobs --min-free-gib 20
ckgit-admin runner macos-vm status --config /path/to/runner.ini
```

`install` creates a new base directory and a sparse disk with the specified
maximum logical size in GiB. It never overwrites an existing base. Interactive
`setup` boots that base with networking to complete macOS and build-tool setup;
it modifies the base. CI execution will use disposable clones with no network
device, not the interactive setup mode. The `jobs` directory must already
exist, be owned by the runner with mode 0700, and be on the same APFS volume as
the base. Configuration checks that the volume is mounted and has enough space
for the disk's full logical size plus the requested free-space reserve.
The base directory must also be owned by the runner and have mode 0700, because
the virtual disk may contain account data and installed tools.
`status` is read-only: it reports available space, reserve, disk logical size,
the filesystem-reported allocated bytes for base components, and counts of
workspace-like and other entries. Allocated bytes are an estimate and may count
blocks shared with APFS clones, so do not add those estimates to infer unique
physical usage. Counts are a snapshot, not
proof that a VM has stopped. It reports insufficient capacity with the available
space and exits nonzero; it changes no storage and starts no runner.
For paths under `/Volumes`, the VM helper also checks the named volume's mount
before install, setup, run and validation; it refuses a stale directory after
an external volume has been unmounted.
Setup holds an exclusive lock on the base; a runner will refuse to clone it
until setup has stopped. Setup also refuses to start while a clone is being
created. Stop the runner before updating the base so jobs cannot start against
the old base during the maintenance window.

The runner admits one job per VM work root, using a persistent runner lock and
an APFS copy-on-write clone for each job. It does not retain a pool of clones or
share host build caches. A normal stop removes the clone after the VM helper
confirms shutdown. After a runner crash, a surviving helper can record a durable
stop confirmation; on restart, the runner recovers that workspace only after
checking the confirmation and acquiring the helper's exclusive lock. A workspace
cleanup preserves that confirmation and lock while removing the bulk of its
contents, so an interruption during that phase can also be recovered. An
interruption during final marker removal, or a workspace without valid stop
confirmation, remains quarantined during the same host boot.
Restart the Mac, remount the configured volume and restart the runner to remove
recognized previous-boot workspaces before accepting work. After VM preflight, the
runner also removes private relay directories carrying its ownership marker.
Startup validates and locks both the VM work root and relay root before it
recovers VM workspaces or creates a preflight clone.
Unknown or unmarked relay data still blocks startup and must be inspected
separately. Do not manually delete a
current-boot workspace just because its helper process has exited. Apple
Developer Technical Support states that the system cleans up after a failed VM
host, but provides neither a cleanup deadline nor a public API for a successor
process to confirm completion ([Apple developer forum discussion](https://developer.apple.com/forums/thread/850709)).
The base and downloaded restore image can consume substantial space; remove the
restore image after the base is verified if it is no longer needed.

For development qualification, the guest service can be installed from a
read-only setup share. After installing the runner component, create a new share
directory and boot the base with it:

```sh
ckgit-admin runner macos-vm prepare-share /Volumes/PRO-BLADE/tmp/ci-setup/guest-share
ckgit-admin runner macos-vm setup /Volumes/PRO-BLADE/tmp/ci-setup/base \
  --setup-share /Volumes/PRO-BLADE/tmp/ci-setup/guest-share
```

`prepare-share` copies the guest binary and installer from the same installed
runner component; it requires a new directory and does not overwrite an existing
share. The VM sees this share read-only. Remove the share after guest setup when
it is no longer needed.
In the guest, create an **empty** work directory owned by the unprivileged CI
user with mode 0700. For example, after creating an appropriate volume inside
the guest:

```sh
mkdir -m 700 /Volumes/PRO-BLADE/tmp/ckgit-vm-work
sudo /bin/sh '/Volumes/My Shared Files/install-vm-guest.sh' \
  '/Volumes/My Shared Files/ckgit-macos-vm-guest' ckgit \
  /Volumes/PRO-BLADE/tmp/ckgit-vm-work
```

The paths under `/Volumes/PRO-BLADE` in this guest command refer to a volume
**inside the virtual disk**, not a host volume. Select a suitable path for the
guest and replace `ckgit` with its unprivileged build account. The installer
refuses to overwrite an existing guest service and installs a launch daemon;
shut down the setup VM before testing a disposable clone. A synthetic offline
job and artifact passed with this setup, but production qualification is still
incomplete. Install the required build tools inside the base during setup;
the guest installer does not install Xcode or other toolchains.

The operator may append non-secret `NAME=VALUE` bindings to the installer
command for tools already installed **inside the guest**, for example
`ORTHOMAP_OPENMVS_BIN=/opt/openmvs/bin`. These bindings are stored in the
root-owned guest launch configuration and passed to each job; workflow values
cannot override them. Execution-control names such as `PATH`, `TMPDIR`,
`CKGIT_*` and `DYLD_*` are refused. All jobs can read these values, so never
put credentials in them. This manual configuration does not install a tool or
constitute automatic dependency-request approval; the operator must verify
and manage the tool separately.

## Update the guest service

When upgrading the installed runner, update its guest binary in the base too.
Stop the host runner first, copy the new `bin/ckgit-macos-vm-guest` into the
read-only setup share and boot the base in setup mode with that share. Inside
the guest, run the following as an administrator:

```sh
sudo /bin/sh <<'SH'
set -eu
source='/Volumes/My Shared Files/ckgit-macos-vm-guest'
installed='/Library/Application Support/ck-git-hosting-vm/ckgit-macos-vm-guest'
plist='/Library/LaunchDaemons/org.ckgit.vm-guest.plist'
test -f "$source" && test -f "$installed"
/usr/bin/plutil -lint "$plist"
if /bin/launchctl print system/org.ckgit.vm-guest >/dev/null 2>&1; then
  /bin/launchctl bootout system "$plist"
fi
/usr/bin/install -o root -g wheel -m 755 "$source" "$installed"
/usr/bin/cmp "$source" "$installed"
SH
```

The existing service configuration and tool bindings remain in place. If any
command fails, resolve the error before restarting the VM or runner; the guest
service may be stopped or the binary may need to be reinstalled. Shut down the
setup VM and validate a disposable job before restarting the host runner. The
first-install script intentionally refuses to overwrite an existing service.

## Connect to the main host

Prepare a dedicated, unprivileged runner account and an SSH key on the Mac,
pin the main host's verified SSH host key, and create a private `runner.ini`.
The [remote runner guide](ci-remote-runners.md) explains the key and host-key
steps. Keep the key and `known_hosts` outside `ci_build_root` and the VM work
root. For example, the relevant configuration fields are:

```ini
schema_version=1
server=ckgit@main-host
name=mac-vm-builder
identity=/absolute/path/to/runner/id_ed25519
known_hosts=/absolute/path/to/runner/known_hosts
ci_build_root=/Volumes/PRO-BLADE/tmp/ckgit-vm-relay
ci_allow_network=false
ci_macos_vm_base=/Volumes/PRO-BLADE/tmp/ci-setup/base
ci_macos_vm_work_root=/Volumes/PRO-BLADE/tmp/ci-setup/jobs
ci_macos_vm_min_free_gib=20
```

Use an existing empty, private relay directory separate from the base and VM
work root. The `configure` command above writes the VM fields into an existing
runner configuration after checking that the relay directory exists and is
separate from VM storage. It does not register or start the runner. VM mode rejects
`ci_allow_degraded=true`, network access and a shared `ci_cache_root`.
Windows `ci_toolchain_root`, custom `ci_toolchain_paths` and
`ci_toolchain_env.*` settings are also rejected: VM jobs use tools installed
inside the guest, not a toolchain staged on the host.

On the main host, copy the runner **public** key there and register it as the
hosting service account:

```sh
sudo -u ckgit ckgit-admin runner add mac-vm-builder \
  --public-key /path/to/mac-vm-builder.pub --config /etc/ck-git-hosting/server.ini \
  --labels macos-vm,arm64 | sudo tee -a /etc/ck-git-hosting/authorized_keys
sudo -u ckgit ckgit-admin runner list --config /etc/ck-git-hosting/server.ini
```

The `add` command prints the restricted SSH authorized-key entry. Use your
installation's configured authorized-keys path if it differs from this example.
After qualifying the installation, run the following as the Mac runner account.
This command connects to the main host and **can claim jobs**:

```sh
ck-ci-runnerd serve --remote --config /absolute/path/to/runner.ini
```

Always register the VM runner with `macos-vm`: this label prevents it from
claiming unlabelled legacy jobs. Labels route work; they do not restrict which
project may request the runner. Target it with `runs_on: [macos-vm, arm64]`. A failed
VM job stops the runner before it claims another job; inspect its output and
the work root before restarting. The web CI view reports runs and artifacts;
runner registration and configuration remain administrator CLI operations.
