# Server installation and pairing

This guide installs the `ck-git-hosting` server on a Debian 13 host with
systemd, pairs the first device key, and shows how to remove the product
again without losing a repository.

## What the installer changes

`packaging/install.sh` prints every change first and applies nothing until
`--yes` is given or an interactive confirmation is answered. `--dry-run`
stops after the list.

| Path | Owner and mode | Purpose |
|---|---|---|
| `ckgit` system account | locked password, shell `/bin/sh` | runs the daemon, Git services, and the hook |
| `/usr/bin/ck-git-hostingd`, `ck-git-shell`, `ckgit-admin` | `root:root 0755` | server binaries |
| `/usr/lib/ck-git-hosting/hooks/post-receive` | `root:root 0755` | shared compiled receive hook |
| `/etc/ck-git-hosting/server.ini` | `root:ckgit 0640` | daemon configuration; kept on reinstall |
| `/etc/ck-git-hosting/authorized_keys` | `root:root 0644` | paired device keys; kept on reinstall |
| `/srv/ck-git-hosting/repos` | `ckgit:ckgit 0750` | bare repositories |
| `/var/lib/ck-git-hosting` | `ckgit:ckgit 0750` | service home |
| `/var/lib/ck-git-hosting/state` | `ckgit:ckgit 0700` | private checkout metadata and event log |
| `/etc/systemd/system/ck-git-hosting.service` | `root:root 0644` | hardened unit |
| `/etc/ssh/sshd_config.d/ck-git-hosting.conf` | `root:root 0644` | `Match User ckgit` block |
| `/run/ck-git-hosting/control.sock` | created by the unit | same-user control socket |

The account keeps `/bin/sh` deliberately: sshd runs a forced command through
the account's shell, so `nologin` would break Git. The forced command and the
`restrict` option in every `authorized_keys` entry are what deny a shell, a
PTY, forwarding, and any command other than the documented Git and RPC forms.

The sshd drop-in moves the account's `AuthorizedKeysFile` to the root-owned
`/etc/ck-git-hosting/authorized_keys`, so a compromised daemon cannot add a
device key. The installer writes the drop-in only when `/etc/ssh/sshd_config`
includes `sshd_config.d`, validates the complete configuration with `sshd -t`,
and removes the drop-in again if validation fails. `--no-sshd` skips this
step; add the `Match User ckgit` block by hand in that case.

## Install

Build on the target or copy a build directory for the same architecture:

```text
make BUILD_DIR=/path/to/build all
sudo sh packaging/install.sh --build-dir /path/to/build --dry-run
sudo sh packaging/install.sh --build-dir /path/to/build --yes
```

Options:

- `--http-port PORT` writes `http_port=PORT` into a new `server.ini`; the
  dashboard binds `127.0.0.1` only.
- `--no-service` installs the unit without enabling or starting it.
- `--staging DIR` lays out the same tree beneath `DIR` and skips the account,
  systemd, and sshd steps. The test suite uses this on a workstation.

The unit starts `ck-git-hostingd --config /etc/ck-git-hosting/server.ini`.
Check the effective configuration without opening a socket; the file is
readable only by root and the service account, so use `sudo`:

```text
sudo ck-git-hostingd --config /etc/ck-git-hosting/server.ini --check
```

`server.ini` is strict: `schema_version=1`, absolute `repo_root` and
`control_socket`, optional absolute `state_root` and `hook_directory`, and an
optional `http_port`. Unknown keys, relative paths, and duplicates are
rejected, and the daemon refuses to combine `--config` with individual path
options.

Validate the hardening profile on the target before relying on it:

```text
systemd-analyze verify /etc/systemd/system/ck-git-hosting.service
systemd-analyze security ck-git-hosting.service
journalctl -u ck-git-hosting.service -b
```

The unit denies every address except localhost. If a LAN read-only dashboard
is ever enabled deliberately, adjust `IPAddressAllow` together with the daemon
bind address in the same change.

## Pair a device

On the device, create a dedicated key without a passphrase prompt in
automation, for example `ssh-keygen -t ed25519 -f ~/.ssh/ckgit -C mac-studio`.
On the server, generate the exact restricted line and append it:

```text
sudo ckgit-admin authorized-key --client-id mac-studio --public-key mac-studio.pub \
  | sudo tee -a /etc/ck-git-hosting/authorized_keys
```

The generated line is:

```text
restrict,command="/usr/bin/ck-git-shell --client-id mac-studio --repo-root /srv/ck-git-hosting/repos --control-socket /run/ck-git-hosting/control.sock --state-root /var/lib/ck-git-hosting/state" ssh-ed25519 AAAA... mac-studio
```

`ckgit-admin` accepts only a plain single-line public key of a supported type
and replaces the key comment with the client ID. It never edits a file, and it
refuses a path that could not be embedded verbatim inside the quoted command.
Use one key per device; a client ID identifies the device in every event.

Verify from the device:

```text
ssh -i ~/.ssh/ckgit ckgit@server 'ckgit-rpc 1 ping'
ssh -i ~/.ssh/ckgit ckgit@server
```

The first command prints `ok`; the second must be denied because no shell is
allowed. Then configure the client with `server=ckgit@server` and the same
`client_id`.

## Upgrade

Rebuild, then run the installer again with `--yes`. Binaries, the hook, and
the unit are replaced; `server.ini`, `authorized_keys`, repositories, and
state are kept. Restart the service afterwards:

```text
sudo systemctl restart ck-git-hosting.service
```

## Uninstall

```text
sudo sh packaging/uninstall.sh --dry-run
sudo sh packaging/uninstall.sh --yes
```

By default this stops and removes the service, the binaries, the hook
directory, and the sshd drop-in, then validates and reloads sshd. Bare
repositories, `/etc/ck-git-hosting`, `/var/lib/ck-git-hosting`, and the
account stay in place and every repository remains usable with plain Git.
Removal of the remaining pieces requires an explicit flag each:
`--remove-state`, `--remove-repositories`, and `--remove-account`.
