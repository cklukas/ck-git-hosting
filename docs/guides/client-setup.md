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

`client.ini` is a strict, bounded key/value file.  Its required fields are
`schema_version=1`, `client_id`, `display_name`, `server` (`user@host`), and
`remote_name`; it may also contain repeated `scan_root` and `exclude` entries.
`ckgit status` recognizes only a remote whose name, SSH user, and host exactly
match that configuration.  It compares local branch/tag object IDs against the
paired server without fetching, changing a checkout, or guessing whether a
mismatched tip is fast-forwardable.

## SSH connections

Git and SSH children of the client run in their own session with standard
input from `/dev/null`: they can never wait on a terminal prompt, so the
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
