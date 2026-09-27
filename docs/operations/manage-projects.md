# Manage hosted projects

## Create a repository

`scan` follows no symbolic links, recognizes both `.git` directories and
worktree `.git` files, and never writes to a discovered repository.  Server
repository creation never overwrites an existing path and enables Git's
non-fast-forward and delete protection.  `ck-git-shell` is intended only as an
OpenSSH forced command: it consumes `SSH_ORIGINAL_COMMAND`, rejects shell
syntax and traversal, and can invoke only `git-upload-pack`,
`git-receive-pack`, or the documented local control RPC.

## Checkout registration and metadata

`ckgit register` records the current paired checkout in the private local
inventory and on the server, using the path privacy mode in `client.ini` for
the server report. Registration requires a daemon started with
`--state-root`: this pre-created directory must be owned by the daemon user and
mode `0700`. It stores a strict, atomic metadata record separately from Git
repositories. Registration requires an existing, non-symlink hosted repository.
`--http-port` serves the dashboard on `127.0.0.1` only; its pages accept only
bounded, bodyless HTTP/1.1 `GET` and `HEAD`
requests.

Successful confirmed publishing refreshes that registration after its Git push.
A non-dry-run `ckgit sync` refreshes registration after its atomic preflight and
before pushing refs; if registration fails, it skips the push. `--dry-run` does
not alter registration metadata or Git refs.

The same private state root keeps event logs for server project creation,
checkout registrations, and pushes. The active log rotates before exceeding
64 KiB; repeated rotations in one month receive distinct archive names.
Project pages read the newest two logs and show those
events without writing a path or remote URL into the event log. Set
`--hook-directory` to an administrator-provisioned, non-symlink directory that
contains the compiled executable `post-receive` hook. New repositories then use
that directory as Git's hooks path. When `ck-git-shell` is also given the same
private `--state-root`, it passes the authenticated client ID, validated
project name, repository/state-root paths, and control socket to `git-receive-pack`; the
hook records a bounded `git-push` event after successful ref updates and asks
the daemon to refresh that project. The refresh transport has a two-second
deadline and a failure does not fail the push.

## Remove a hosted project

Run `ckgit-admin remove-project NAME --config SERVER-CONFIG --dry-run` (or
`--repo-root ROOT --state-root ROOT` instead of `--config`) as the account
that owns the private metadata to inspect the operation. Omitting `--dry-run`
prints the same plan and then asks for confirmation on a terminal ("Execute
this plan? Type yes:"), or requires `--yes` for unattended use; redirected
input without `--yes` only previews. Removal renames `NAME.git` into
`ROOT/.trash/NAME.git.<epoch>` with a unique suffix when needed. It clears the
project's checkout registrations, derived metadata, and entries in every event
archive. Existing trash is preserved; this command does not permanently erase
the retained repository.

Provide `--control-socket PATH` to notify a running daemon immediately. The
periodic sweep also drops deleted projects and removes orphan metadata for
repositories deleted by hand. Creation, registered-checkout writes, removal,
event writes, and orphan cleanup share a repository lifecycle lock. Late push
events for a project removed administratively are discarded. There is no HTTP
delete route or automatic trash purge.
