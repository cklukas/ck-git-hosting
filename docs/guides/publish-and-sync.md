# Publish and sync projects

## Publish a project

```sh
cd ~/code/my-project
ckgit publish --dry-run
ckgit publish
```

`ckgit create` requests a validated empty bare repository from the paired
server. It does not alter a local checkout; use `ckgit clone` after creation.

Inside a Git repository, `ckgit publish` previews by default. With `--yes`, it
creates the paired project only when the configured remote name is absent,
adds only that remote, and pushes all local branches and tags atomically
without force or deletion refspecs. `--branch NAME` (repeatable) limits the
push to those local branches, and `--no-tags` leaves tags out. Repeated branch
names are counted once. Existing project identity comes from the validated
configured remote, so a custom publication name or clone destination keeps
working. `--name` must agree with that identity; an incompatible server remote
causes a failure without being overwritten.

Both `publish` and `sync` show the project and server, source folder, default
branch for creation, new/differing/unchanged/excluded refs, server-only refs
retained, uncommitted entries excluded, and the proposed main-checkout and
server path report. `--verbose` expands every ref name in long lists. A differing
tip is identified as needing Git's preflight; it is not presented as a verified
permissible update. The non-forcing preflight checks an existing project's
actual update before execution. `--dry-run` shows this plan without changing
refs or checkout management. When no selected refs differ, publication only
refreshes management.

Outside a Git repository, `ckgit publish` checks the current folder's immediate
subdirectories, lists unpublished projects in name order, and asks for one
confirmation before publishing them. An explicit folder argument does the same:

```text
ckgit publish /path/to/projects
ckgit publish /path/to/projects --dry-run
ckgit publish /path/to/projects --yes
```

It does not recurse, follow symbolic links, or publish bare repositories.
Already paired projects and names already present on the server are skipped;
use `ckgit sync` to update previously registered projects. `--branch` and
`--no-tags` apply to each candidate, and a project missing a requested branch
is reported and skipped. `--name` and `--replace-checkout` require a single repository.
`--dry-run` always previews without publishing, even with `--yes`. Without a
terminal, omit `--yes` to preview or supply it to publish without a prompt.

## Sync committed changes

```sh
ckgit sync --repo . --dry-run
ckgit sync --repo .
```

`ckgit sync` preflights Git's atomic branch/tag update before pushing. It has no
force or deletion refspecs, never writes a commit, and reports dirty worktree
entries without transferring their uncommitted contents. It uploads committed
local changes; it does not fetch, pull, or merge changes from another device.
By default, it uses the private managed-project inventory, supplemented by
legacy server reports. It requires no scan roots for managed folders. Missing
folders, stale repository roots, incompatible remotes, and unresolved legacy
paths remain visible and are skipped with recovery advice. Other projects
continue when one needs attention.
To add branches and tags omitted by an earlier publish across all registered
projects, run `ckgit sync --dry-run` to preview, then `ckgit sync`. Each sync
includes all local branches and tags; earlier `publish --branch` or
`--no-tags` choices are not retained. Remote-tracking branches such as
`origin/feature` are not local branches and are not published.

Every `sync` and confirmed `publish` holds an advisory lock on `sync.lock`
beside `client.ini`. A second run for the same configuration exits with code
`4` immediately instead of queueing behind an unknown transfer.
