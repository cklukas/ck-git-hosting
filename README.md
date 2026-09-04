# ck-git-hosting

`ck-git-hosting` is a small, private Git control plane for a trusted LAN.  It
keeps Git transport in Git and OpenSSH, while adding safe local inventory,
checkout registration, and a deliberately small web dashboard.

The current implementation covers the first three vertical slices:

- a local audit client that discovers working trees without modifying them and
  reports local refs, remotes, current branch, and dirty-file counts;
- a local server foundation that creates standard bare repositories with safe
  receive defaults, serves a small same-user control socket, dispatches only
  exact Git SSH service commands, and exposes an optional loopback dashboard;
- privacy-aware checkout registration in a separate private state directory,
  shown as last-reported client/path data in the dashboard.

It now also ships as a service installation: a strict `server.ini`, a
hardened systemd unit, an installer and uninstaller that print every change
first, and a pairing helper that generates the restricted `authorized_keys`
line for a device. The client selects one canonical checkout per project and
holds a per-configuration lock so scheduled and manual runs never overlap.
Administrative web forms and the migration tooling remain later work packages.

## Build and test

The source tree is never used for build products.  Choose a new directory under
`/Volumes/PRO-BLADE/tmp` for each build:

```text
make BUILD_DIR=/Volumes/PRO-BLADE/tmp/ck-git-hosting-my-build
make BUILD_DIR=/Volumes/PRO-BLADE/tmp/ck-git-hosting-my-build check
```

On another machine, or in CI, pass `BUILD_ROOT` explicitly; the build
directory must lie beneath it and the test suite keeps every scratch file
there as well:

```text
make BUILD_ROOT=/tmp/ck BUILD_DIR=/tmp/ck/build all check
```

## Current command

```text
ckgit scan ROOT [ROOT ...]
ckgit scan --json ROOT [ROOT ...]
ckgit scan --config /path/to/client.ini
ckgit config show --config /path/to/client.ini
ckgit status --config /path/to/client.ini [--repo PATH]
ckgit clone --config /path/to/client.ini NAME [DESTINATION]
ckgit create --config /path/to/client.ini NAME [--default-branch BRANCH]
ckgit publish --config /path/to/client.ini [--name NAME] [--yes] [REPOSITORY]
ckgit register --config /path/to/client.ini [--repo PATH]
ckgit sync --config /path/to/client.ini [--repo PATH] [--dry-run]
ckgit checkout list --config /path/to/client.ini [NAME]
ckgit checkout set-canonical --config /path/to/client.ini NAME PATH

ckgit-admin create NAME --repo-root ROOT [--default-branch BRANCH] [--hook-directory PATH] [--dry-run]
ckgit-admin authorized-key --client-id ID --public-key FILE [--shell PATH] [--repo-root ROOT] [--control-socket PATH] [--state-root ROOT]
ck-git-hostingd --repo-root ROOT --control-socket PATH [--state-root ROOT] [--hook-directory PATH] [--http-port PORT] [--check]
ck-git-hostingd --config /etc/ck-git-hosting/server.ini [--check]
ck-git-shell --client-id ID --repo-root ROOT --control-socket PATH [--state-root ROOT]
```

`ckgit` exit codes are stable: `0` success, `1` error, `2` usage, `3` partial
result or attention needed, and `4` when another sync or publish for the same
configuration holds the lock.

`scan` follows no symbolic links, recognizes both `.git` directories and
worktree `.git` files, and never writes to a discovered repository.  Server
repository creation never overwrites an existing path and enables Git's
non-fast-forward and delete protection.  `ck-git-shell` is intended only as an
OpenSSH forced command: it consumes `SSH_ORIGINAL_COMMAND`, rejects shell
syntax and traversal, and can invoke only `git-upload-pack`,
`git-receive-pack`, or the documented local control RPC.

`client.ini` is a strict, bounded key/value file.  Its required fields are
`schema_version=1`, `client_id`, `display_name`, `server` (`user@host`), and
`remote_name`; it may also contain repeated `scan_root` and `exclude` entries.
`ckgit status` recognizes only a remote whose name, SSH user, and host exactly
match that configuration.  It compares local branch/tag object IDs against the
paired server without fetching, changing a checkout, or guessing whether a
mismatched tip is fast-forwardable.

`ckgit clone` uses only the configured `user@host` plus a validated project
name, creates the destination only when it does not already exist, and names
the resulting remote from `remote_name` (normally `ckgit`).

`ckgit create` requests a validated empty bare repository from the paired
server. It does not alter a local checkout; use `ckgit clone` after creation.

`ckgit publish` previews by default. With `--yes`, it creates the paired
project only when the configured remote name is absent, adds only that remote,
and pushes all local branches and tags atomically without force or deletion
refspecs. Existing remotes with another URL are left unchanged and cause a
failure.

`ckgit sync` preflights Git's atomic branch/tag update before pushing. It has no
force or deletion refspecs, never writes a commit, and reports dirty worktree
entries without transferring their uncommitted contents. Without `--repo`, it
scans configured roots and syncs one checkout per project: the selected
canonical checkout, or the only checkout when there is no duplicate. A project
with duplicate checkouts and no selection is skipped with a proposal; a
project whose selected checkout is unavailable is skipped entirely. `--repo`
syncs the named checkout explicitly, canonical or not.

`ckgit checkout list` groups discovered checkouts by paired project and marks
each as `single`, `canonical`, `proposed`, or `other`, with `[linked worktree]`
and `[ephemeral name]` hints. `ckgit checkout set-canonical NAME PATH` verifies
that `PATH` is a working tree paired with `NAME` and records it in
`canonical.ini` beside `client.ini`; the proposal is never applied on its own.

Every `sync` and confirmed `publish` holds an advisory lock on `sync.lock`
beside `client.ini`. A second run for the same configuration exits with code
`4` immediately instead of queueing behind an unknown transfer.

`ckgit register` records the current paired checkout on the server, using the
path privacy mode in `client.ini`. Registration requires a daemon started with
`--state-root`: this pre-created directory must be owned by the daemon user and
mode `0700`. It stores a strict, atomic metadata record separately from Git
repositories. `--http-port` serves the project list and project detail pages on
`127.0.0.1` only; both accept only bounded, bodyless HTTP/1.1 `GET` and `HEAD`
requests.

Successful `ckgit publish --yes` refreshes that registration after its Git push.
A non-dry-run `ckgit sync` refreshes registration after its atomic preflight and
before pushing refs; if registration fails, it skips the push. `--dry-run` does
not alter registration metadata or Git refs.

The same private state root keeps a bounded append-only event log for server
project creation and checkout registrations. The project page shows those
events without writing a path or remote URL into the event log. Set
`--hook-directory` to an administrator-provisioned, non-symlink directory that
contains the compiled executable `post-receive` hook. New repositories then use
that directory as Git's hooks path. When `ck-git-shell` is also given the same
private `--state-root`, it passes only the authenticated client ID, validated
project name, and state-root path to `git-receive-pack`; the hook records a
bounded `git-push` event after successful ref updates.

## Install as a service

[docs/operations/01-installation.md](docs/operations/01-installation.md)
describes the packaged layout, the hardened systemd unit, the sshd drop-in that
moves the `ckgit` account's authorized keys to a root-owned file, device
pairing with `ckgit-admin authorized-key`, upgrade, and removal. In short:

```text
make BUILD_DIR=/Volumes/PRO-BLADE/tmp/ck-git-hosting-my-build all
sudo sh packaging/install.sh --build-dir /Volumes/PRO-BLADE/tmp/ck-git-hosting-my-build --dry-run
sudo sh packaging/install.sh --build-dir /Volumes/PRO-BLADE/tmp/ck-git-hosting-my-build --yes
```

The daemon then reads `/etc/ck-git-hosting/server.ini`; `--check` prints the
effective configuration without opening a socket. Uninstalling keeps every
repository, the configuration, and the metadata unless their explicit removal
flags are given.

Released builds are packaged by GitHub Actions: Debian 13 `.deb` files for
arm64 (Raspberry Pi) and amd64, distro-neutral Linux tarballs, macOS client
tarballs, and a Homebrew formula served from this repository as a tap. See
[docs/operations/02-packages-and-releases.md](docs/operations/02-packages-and-releases.md).

The exact version-1 SSH/control contract is in
[docs/protocol/01-ssh-and-control-v1.md](docs/protocol/01-ssh-and-control-v1.md).

See [the planning set](docs/planning/README.md) for the staged product design.

## License

Licensed under the MIT License, see [LICENSE](LICENSE).
