# Backup and recovery

Run these commands as the account that owns the hosted repositories and private
state, normally `ckgit`. Use a private backup location on a separate disk and
keep an additional copy off the server. Every command supports `--help`.

## Save and verify a backup

```sh
ckgit-admin backup --config /etc/ck-git-hosting/server.ini --output /backup/ckgit-2026-09-05 --dry-run
ckgit-admin backup --config /etc/ck-git-hosting/server.ini --output /backup/ckgit-2026-09-05 --yes
ckgit-admin verify-backup /backup/ckgit-2026-09-05
```

The output directory must not already exist and must be outside the repository
and state roots. Without `--yes`, an interactive terminal asks for confirmation;
redirected input prints a preview. `--dry-run` always validates without writing.
Git verification can take time on large repositories.

A version-1 backup contains independent Git mirrors, every hosted ref including
tags and custom namespaces, and HEAD, including unborn or detached HEAD. It also
contains private checkout registrations, all event archives, derived project
metadata, and any CI run records and step logs the runner kept under the same
state root. Original repository configuration is saved separately under
`configs/PROJECT.config` for review. Files have SHA-256 checksums in
`manifest.sha256`; verification checks the complete file inventory, Git object
integrity, and metadata. Checksums detect corruption; they are not a signature
proving who supplied a backup. Keep backups private and use a trusted copy.

Backups contain objects reachable from refs and HEAD. They do **not** preserve
reflogs, unreachable or deleted Git objects, repository trash, SSH keys,
`authorized_keys`, operating-system configuration, service definitions, or
credentials. Back up those items separately using your normal system backup.
The exact original Git config may contain private values, which is another reason
to protect the backup directory. Empty repositories are included.

Repository lifecycle and metadata operations are locked during capture. Pushes
can still transfer Git objects and update refs. The backup compares all source
refs, HEAD, and config before and after capture and rejects changes; retry while
Git writes are stopped if the server is busy. A completed push's event may lag its
Git refs while the lifecycle lock is held. The backup preserves valid Git history
and a consistent metadata copy rather than promising one simultaneous timestamp
for both. Symlinks, external object dependencies in mirrors, malformed metadata,
and metadata without a hosted project are rejected.

## Restore a complete server backup

Provision new repository and private state directories with the service account's
ownership. The state directory must be empty and mode `0700`. Existing project
destinations are never overwritten. Keep the destination unavailable to Git
clients until verification and restoration finish.

```sh
ckgit-admin restore-backup /backup/ckgit-2026-09-05 --config /etc/ck-git-hosting/server.ini --dry-run
ckgit-admin restore-backup /backup/ckgit-2026-09-05 --config /etc/ck-git-hosting/server.ini --yes
```

Alternatively, supply `--repo-root ROOT --state-root ROOT`. Do not combine those
root options with `--config`. `--hook-directory PATH` selects or overrides the
destination hook directory; it must contain an executable `post-receive` file.
Repository and state roots must be separate, non-overlapping directories. They
may be on different filesystems, and neither may be inside the backup.

Restore verifies the backup and its staged copy before publishing anything.
It installs fresh receive safety settings and the destination's configured hooks.
It does not activate the original Git config, mirror remotes, or backed-up hooks.
Review `configs/PROJECT.config` separately if additional settings are needed.
When no hook directory is configured, no custom receive hook is installed.
SSH authorization and client configuration must be restored separately.

Publication keeps the existing root directory inodes so lifecycle locks remain
valid. A failed installation attempts to move its installed entries back out and
retains the staged recovery data, with its exact location in the error message.
Review any rollback warning before reopening Git access; do not delete retained
recovery data until the destination and backup are verified. A successful restore
requests an index refresh when `--config` supplies a control socket; otherwise
the regular dashboard sweep discovers the projects.

## Recover a removed project

`remove-project` retains the Git repository in `.trash` and removes its metadata.
Like backup and restore, it previews its plan first and then asks for
confirmation on a terminal or `--yes`; redirected input without `--yes` only
previews. List trashed copies, then restore an entry by the exact displayed
name:

```sh
ckgit-admin trash list --config /etc/ck-git-hosting/server.ini
ckgit-admin restore-project PROJECT.git.TIMESTAMP --config /etc/ck-git-hosting/server.ini --dry-run
ckgit-admin restore-project PROJECT.git.TIMESTAMP --config /etc/ck-git-hosting/server.ini --yes
```

Use `--name OTHER-NAME` to restore under a different unused name. For an entry
whose name begins with a dash, put all options first and add `--` before the
entry. Recovery preserves the trash copy and restores Git only; deleted checkout
registrations and events stay deleted. The restored project uses fresh hosting
settings and the destination hooks, just like full restore. Clients can register
their existing paired checkouts again after confirming the restored project.
