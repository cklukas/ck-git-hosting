---
description: Install ckgit on a device, pair it with your server, and check the connection with ckgit setup and ckgit doctor.
---
# Set up the client

Install [the client package](../operations/02-packages-and-releases.md), then [pair its SSH key](../operations/01-installation.md#pair-a-device) with your server.

## Connect your device

```sh
ckgit setup --server ckgit@server --client-id laptop --web-host server
ckgit doctor
```

On a new computer, run `ckgit setup`, then `ckgit doctor`. Setup previews and
saves a private configuration; it requires `--overwrite` before replacing an
existing file. The device ID must match its paired SSH key. The Git account
(`server=ckgit@rpi4`, for example) and ordinary dashboard SSH login
(`web_host=rpi4`) are separate settings. Optional `web_port` defaults to 8420.
Doctor checks Git, the restricted control connection, and an actual dashboard
HTTP request through a temporary tunnel, then closes it. SSH keys and trusted
host keys must already be configured; diagnostic failures explain the next step.

## Configuration

On Windows, the x64 client installer bundles `ckgit.exe` and `ckdocs.exe`.
Choose installation for the current user without elevation, or for all users
with administrator rights. The portable ZIP requires no installation. Git
for Windows and an OpenSSH client must be available on PATH; the installer
does not install Git, generate SSH keys or change trusted host keys.

The Windows default configuration is
`%LOCALAPPDATA%\ck-git-hosting\client.ini`. Use `--config` to select another
location, including when development data must stay on a dedicated drive:

```powershell
ckgit setup --config V:\ckgit\client.ini --server ckgit@server --client-id workstation --web-host server --scan-root V:\projects
ckgit doctor --config V:\ckgit\client.ini
```

Windows configuration and checkout-selection files use UTF-8, absolute drive
paths, and a private owner/SYSTEM ACL. Setup writes LF line endings; the strict
configuration reader requires those line endings. `~` in setup paths expands
to the Windows user profile. Reopen a terminal after changing PATH during
installation.

`client.ini` is a strict, bounded key/value file.  Its required fields are
`schema_version=1`, `client_id`, `display_name`, `server` (`user@host`), and
`remote_name`; it may also contain repeated `scan_root` and `exclude` entries.
`ckgit status` recognizes only a remote whose name, SSH user, and host exactly
match that configuration.  It compares local branch/tag object IDs against the
paired server without fetching, changing a checkout, or guessing whether a
mismatched tip is fast-forwardable.

## SSH connections

Git and SSH children of the client run in their own session with standard
input from `/dev/null` (`NUL` on Windows): they can never wait on a terminal prompt, so the
server's host key must already be known and a passphrase-protected device key
needs an agent. Unless `GIT_SSH_COMMAND`, `GIT_SSH`, or `core.sshCommand` is
already configured, transfers use `ssh` with batch mode, a connect timeout,
and keepalives, so a dead connection fails within minutes. Transfers show
Git's own progress when standard error is a terminal and are bounded by a
four-hour limit; control round trips by one minute; local inspection by two
minutes. When a limit is hit, or when `ckgit` itself is interrupted or
terminated, the whole child process tree is ended, so no orphaned push keeps
running, and failure messages name the repository and Git's most specific
line. Discovery ignores a `.git` directory without `HEAD` (leftover debris)
and refuses a stale `.git` entry that Git would resolve to a surrounding
repository.
