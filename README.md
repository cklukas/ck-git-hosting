# ck-git-hosting

`ck-git-hosting` is a small, private Git control plane for a trusted LAN.  It
keeps Git transport in Git and OpenSSH, while adding safe local inventory,
checkout registration, and a deliberately small web dashboard.

The current implementation includes:

- a local audit client that discovers working trees without modifying them and
  reports local refs, remotes, current branch, and dirty-file counts;
- a local server foundation that creates standard bare repositories with safe
  receive defaults, serves a small same-user control socket, dispatches only
  exact Git SSH service commands, and exposes an optional loopback dashboard;
- privacy-aware checkout registration in a separate private state directory,
  shown as last-reported client/path data in the dashboard;
- an indexed, read-only dashboard with last commits, rendered READMEs, file
  browsing, commit history and diffs, calendar views, and a commit graph;
- administrative project removal that retains the bare repository in trash
  and clears its metadata, plus cleanup for repositories deleted by hand;
- opt-in per-project continuous integration that runs a repository's own
  `.ckgit/ci.yml` in a sandboxed runner on push, with read-only status and
  step logs in the dashboard.

It now also ships as a service installation: a strict `server.ini`, a
hardened systemd unit, an installer and uninstaller that print every change
first, and a pairing helper that generates the restricted `authorized_keys`
line for a device. The client keeps a private inventory with one main checkout
per project on each device and holds a per-configuration lock so scheduled and
manual uploads never overlap. Legacy checkout reports can be migrated into
that inventory without exposing full paths to the server.

## Install

macOS, with Homebrew:

```text
brew tap cklukas/ck-git-hosting https://github.com/cklukas/ck-git-hosting
brew install --HEAD ckgit
```

`brew install ckgit` (without `--HEAD`) and the Debian/tarball packages
become available once a release is tagged; see
[docs/operations/02-packages-and-releases.md](docs/operations/02-packages-and-releases.md)
for every install path, including the server.

Online documentation: <https://cklukas.github.io/ck-git-hosting/>.

## Build and test

The source tree is never used for build products; every build directory must
lie beneath a build root, and the test suite keeps every scratch file there
too. This is the portable form, explicit about both and safe to copy verbatim
onto another machine or into another project's own CI:

```text
make BUILD_ROOT=/tmp/ck BUILD_DIR=/tmp/ck/build all check
```

For everyday local use, `BUILD_ROOT` defaults to your system's temporary
directory (`TMPDIR`, or `/tmp`), so a bare `BUILD_DIR=` works unmodified on a
fresh clone; choose a new directory under the root for each build:

```text
make BUILD_DIR=/tmp/ck-git-hosting-my-build
make BUILD_DIR=/tmp/ck-git-hosting-my-build check
```

To pin a different default for one particular checkout instead of typing
`BUILD_ROOT=` every time — for example to keep build products off a
network-mounted source volume — copy `local.mk.example` to `local.mk`
(gitignored) and set `BUILD_ROOT` there.

`ckgit --version` identifies the release version and Git revision, including
tracked changes. A source archive with no `.git` (a `git archive` export, or
this project's own self-hosted CI checkout) instead reads the commit
`export-subst` stamps into `.ckgit/build-commit`, falling back to a plain
`+source` suffix only if that is unavailable too. Packaging can supply
`CKGIT_BUILD_VERSION=0.1.0` or another exact build identifier to `make`.

This project builds, tests, and releases itself through its own CI and
release mechanism — see
[Deploying ck-git-hosting's own release](docs/operations/04-ci-cd.md#deploying-ck-git-hostings-own-release)
and the [self-hosted release and deploy guide](docs/operations/05-releases-and-deploy.md)
for the end-to-end flow from a pushed tag to an upgraded server.

## Command line

```text
ckgit --help
ckgit help publish
ckgit publish --help
ckgit checkout migrate -h
ckgit --version
ckgit version

ckgit setup [--server USER@HOST] [--client-id ID] [--web-host HOST] [--yes] [--overwrite] [--dry-run]
ckgit doctor [--web-host HOST] [--remote-port PORT] [--timeout SECONDS]
ckgit projects [--json] [--uncloned] [--filter TEXT]
ckgit scan ROOT [ROOT ...]
ckgit scan --json ROOT [ROOT ...]
ckgit scan
ckgit config show
ckgit status [--repo PATH | --scan]
ckgit clone NAME [DESTINATION]
ckgit clone --all --into DIR [--dry-run] [--yes]
ckgit clone --project NAME [--project NAME ...] --into DIR [--dry-run] [--yes]
ckgit fetch [--repo PATH | --project NAME | --all] [--branch NAME ...] [--no-tags] [--dry-run] [--verbose]
ckgit update [--repo PATH | --project NAME | --all] [--dry-run] [--verbose]
ckgit create NAME [--default-branch BRANCH]
ckgit publish [--name NAME] [--branch NAME ...] [--no-tags] [--replace-checkout] [--yes] [--dry-run] [--verbose] [REPOSITORY_OR_FOLDER]
ckgit register [--repo PATH] [--replace-checkout]
ckgit sync [--repo PATH | --scan] [--replace-checkout] [--dry-run] [--verbose]
ckgit checkout list [--scan] [NAME]
ckgit checkout set-canonical NAME PATH
ckgit checkout migrate [--dry-run]
ckgit checkout forget NAME [--dry-run] [--yes]
ckgit web [--config PATH] [--port PORT] [--remote-port PORT] [--project NAME] [--no-open] [ADMIN-HOST]
ckgit release list PROJECT [--json]
ckgit release download PROJECT [--tag TAG] [--asset NAME] [--into DIR]
ckgit completion bash
ckgit completion zsh

ckgit-admin create NAME --config SERVER-CONFIG [--default-branch BRANCH] [--hook-directory PATH] [--dry-run]
ckgit-admin remove-project NAME --config SERVER-CONFIG [--control-socket PATH] [--dry-run] [--yes]
ckgit-admin backup --output NEW-DIR --config SERVER-CONFIG [--dry-run] [--yes]
ckgit-admin verify-backup BACKUP
ckgit-admin restore-backup BACKUP --config SERVER-CONFIG [--dry-run] [--yes]
ckgit-admin trash list --config SERVER-CONFIG
ckgit-admin restore-project TRASH-ENTRY --config SERVER-CONFIG [--name NAME] [--dry-run] [--yes]
ckgit-admin authorized-key --client-id ID --public-key FILE [--shell PATH] [--repo-root ROOT] [--control-socket PATH] [--state-root ROOT]
ckgit-admin ci enable|disable|status NAME --config SERVER-CONFIG
ckgit-admin ci runs NAME --config SERVER-CONFIG
ckgit-admin ci log NAME RUN [STEP] [--follow] --config SERVER-CONFIG
ckgit-admin ci cancel NAME RUN --config SERVER-CONFIG
ck-git-hostingd --repo-root ROOT --control-socket PATH [--state-root ROOT] [--hook-directory PATH] [--http-port PORT] [--check]
ck-git-hostingd --config /etc/ck-git-hosting/server.ini [--check]
ck-git-shell --client-id ID --repo-root ROOT --control-socket PATH [--state-root ROOT]
ck-ci-runnerd serve --config /etc/ck-git-hosting/server.ini [--once]
ck-pagesd serve --config /etc/ck-git-hosting/server.ini
ck-pagesd check --config /etc/ck-git-hosting/server.ini

ckdocs build [--root DIR] [--source DIR] [--config FILE] [--out DIR] [--clean] [--strict] [--quiet]
ckdocs check [--root DIR] [--source DIR] [--config FILE] [--quiet]
```

`--config` defaults to `~/.config/ck-git-hosting/client.ini` (or the
`XDG_CONFIG_HOME` equivalent) when that file exists, so daily commands need no
options at all. It may appear before or after a command, including `config
show`. Both `--option VALUE` and `--option=VALUE` are accepted. Use `--` before
a positional path that starts with a hyphen.

Every command and nested command supports `--help`, `-h`, and `ckgit help
COMMAND [SUBCOMMAND]`. Help explains its scope, defaults, effects, arguments,
options, examples, and exit codes. Help and `--version` work offline without a
configuration file. Argument errors name the problem and point to the relevant
help page. Completion uses the same command definitions; load it with
`source <(ckgit completion bash)` or, after zsh's `compinit`,
`source <(ckgit completion zsh)`.

`ckgit` exit codes are stable: `0` success, `1` error, `2` usage, `3` partial
result or attention needed, and `4` when another sync or publish for the same
configuration holds the lock. Checkout selection, registration, and cloning
share that lock when changing the same managed inventory.

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

On a new computer, run `ckgit setup`, then `ckgit doctor`. Setup previews and
saves a private configuration; it requires `--overwrite` before replacing an
existing file. The device ID must match its paired SSH key. The Git account
(`server=ckgit@rpi4`, for example) and ordinary dashboard SSH login
(`web_host=rpi4`) are separate settings. Optional `web_port` defaults to 8420.
Doctor checks Git, the restricted control connection, and an actual dashboard
HTTP request through a temporary tunnel, then closes it. SSH keys and trusted
host keys must already be configured; diagnostic failures explain the next step.

`ckgit projects` lists hosted projects and their local management state.
`--uncloned` selects projects absent from this device's management records; it
does not search the disk for ordinary Git clones. JSON includes a stable schema
version, project state, selected path, and SSH clone URL.

To populate an existing folder, use `ckgit clone --all --into /path/to/projects`.
The command displays its scope and asks for confirmation on a terminal;
`--dry-run` previews and `--yes` executes without prompting. Repeated
`--project NAME` selects a smaller set. Already managed projects are skipped,
existing folders are never overwritten, and failures do not stop other clones.
Missing or unresolved management records remain visible: repair the selection
with `checkout set-canonical` or `migrate`, or `forget` it before cloning anew.

`ckgit clone` uses only the configured `user@host` plus a validated project
name, creates the destination only when it does not already exist, and names
the resulting remote from `remote_name` (normally `ckgit`). If this device has
no other main checkout for that project, the clone joins ordinary `status` and
`sync` automatically, including with a custom destination name. If another
main checkout already exists, the new folder is a secondary copy; the result
names the main folder and prints the exact command to select the new copy.

`ckgit fetch` downloads all hosted branches into the configured remote-tracking
namespace and downloads tags, without changing local branches or working files.
`--branch NAME` and `--no-tags` limit that operation. `ckgit update` downloads
the current branch and tags, then fast-forwards only a clean, attached checkout.
It never switches branches, rebases, or creates a merge commit. Dirty files,
diverged history, missing branches, and conflicting tags require attention;
other projects continue. Both commands use managed checkouts by default, accept
`--repo PATH` or `--project NAME`, and offer read-only `--dry-run`. When incoming
objects are absent locally, a preview explains that ancestry requires a fetch.
`sync` continues to upload committed local refs only.

`ckgit checkout forget NAME` previews removal of this device's local selection
and server registration, then asks for confirmation (`--yes` for automation).
It also works when the checkout folder is missing. Files, remotes, commits,
the hosted repository, and other devices' records remain intact. The server
must be reachable; a failed remote removal preserves the local selection.
Use `register` to manage the checkout again. Deliberately running `sync --scan`
can also rediscover paired copies.

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

Every device has one main checkout per managed project. Its absolute local
path is stored privately in mode-`0600` `canonical.ini` beside `client.ini`.
The server receives only the path information allowed by `public_path_mode`;
the default `basename` mode works with ordinary sync and never needs to be
changed to `full` just to find this device's folders. Publishing or explicitly
syncing another folder is refused with a message naming the main one unless
`--replace-checkout` is supplied. `publish`, `sync --repo`, and `register` with
that flag update the same main selection as `checkout set-canonical`. The
inventory and lock are shared by configurations in the same directory. Keep
configurations for different servers or device identities in separate
directories; server reports are separate for each client ID.

`ckgit sync` preflights Git's atomic branch/tag update before pushing. It has no
force or deletion refspecs, never writes a commit, and reports dirty worktree
entries without transferring their uncommitted contents. It uploads committed
local changes; it does not fetch, pull, or merge changes from another device.
By default, it uses the private managed-project inventory, supplemented by
legacy server reports. It requires no scan roots for managed folders. Missing
folders, stale repository roots, incompatible remotes, and unresolved legacy
paths remain visible and are skipped with recovery advice. Other projects
continue when one needs attention.

`status`, default `sync`, and `checkout list` show the same managed project set
and effective main selection. Their output begins with the scope, device, and
server. `--repo .` explicitly selects the current repository for status or sync.
Discovery stays explicit: `status --scan` inspects configured roots;
`checkout list --scan` also lists additional paired copies without selecting
them. `sync --scan` discovers paired checkouts under those roots and obeys
the same main selection as ordinary sync. If no selection exists, it uses a
single discovered checkout; duplicates require an explicit choice. A selected
checkout that is absent from the discovery result is skipped, with no fallback
to a different copy.

To add branches and tags omitted by an earlier publish across all registered
projects, run `ckgit sync --dry-run` to preview, then `ckgit sync`. Each sync
includes all local branches and tags; earlier `publish --branch` or
`--no-tags` choices are not retained. Remote-tracking branches such as
`origin/feature` are not local branches and are not published.

`ckgit checkout set-canonical NAME PATH` verifies that `PATH` is a working tree
connected to `NAME`, then updates the server report and private main selection.
The command spelling is retained for compatibility: "canonical" means the
main checkout on this device. It does not upload commits or remove another copy.

For checkouts registered by an older client, run `ckgit checkout migrate
--dry-run`, then `ckgit checkout migrate`. Existing absolute reports can be
adopted directly after validation. Basename-only reports are matched against
configured `scan_root` directories using the folder name and validated remote.
Only a unique match is adopted. Missing or ambiguous matches remain unresolved
and list how to select a folder with `checkout set-canonical`. Migration never
uploads refs or changes the path privacy setting.

`ckgit web` opens the server's loopback dashboard through an SSH tunnel: it
picks a free local port (8420 when available), starts `ssh -N -L` without a
shell using the administrator's own login on the server host (the restricted
`ckgit` account allows no forwarding), waits until the dashboard answers,
prints the URL, opens the browser, and closes the tunnel again on Ctrl+C.
The configured `web_host` chooses the forwarding login and `web_port` chooses
the dashboard port; explicit `ADMIN-HOST` and `--remote-port` override them.
The remote port must match `http_port` in the server's `server.ini`.
`--project NAME` opens that hosted project's overview directly instead of the
project index.

`ckgit release list PROJECT` shows that project's durable, tag-triggered
releases (tag, commit, creation time, and each asset's name, size, and
checksum) over the same restricted SSH control channel as `refs` and
`refresh` -- no dashboard tunnel needed. `ckgit release download PROJECT`
lists the same way, picks the newest release or `--tag`, picks its only asset
or `--asset`, then opens a tunnel exactly like `web` (the ordinary SSH login,
since the restricted `ckgit` account still allows no forwarding) to fetch it,
verifies its size and checksum against the listing, and writes
`--into DIR/NAME.tar` (default: the current directory) only once that
verification passes. See [Releases](docs/operations/04-ci-cd.md#releases) for
how a release is produced and `packaging/update-cli.sh` for a scripted
example that updates this same CLI from one.

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

Every `sync` and confirmed `publish` holds an advisory lock on `sync.lock`
beside `client.ini`. A second run for the same configuration exits with code
`4` immediately instead of queueing behind an unknown transfer.

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

## Browse projects and history

The project table sorts by the newest commit, with a link to sort by name.
Each overview shows the last commit and author, repository size, dated branch
and tag lists, checkout registrations, recent events, and the default branch's
README. The in-memory index supplies the table and default overview without spawning
Git during a request. Selecting a revision on the overview loads that revision's
commit and README. Startup shows indexing placeholders while a background
thread builds the records. Push notifications queue a refresh; a 60-second
fingerprint sweep also discovers administrative pushes and deleted projects.

Every page includes the text logo and an About dialog with the running build
version, copyright, and license. Native browser controls open and dismiss it.

The Files view has a classic expandable tree with connector lines, folder and
file-type icons, a branch/tag picker, and a preview. Folder controls expand
inline without JavaScript; folder names open the directory preview. The current
path opens automatically, and only the selected item is highlighted. On desktop
the tree panel fills the available screen height, with a 32rem minimum.
Branch, tag, and commit labels distinguish a moving named revision from an
immutable snapshot. Files, breadcrumbs, section navigation, and pagination keep
the selected revision; Permalink pins it, and Latest default branch explicitly
returns to the default revision. Calendar branch/tag choices preserve the month.

READMEs render a built-in Markdown subset in the overview and in directories.
Other Markdown files also open rendered, with a Source mode and heading links.
GitHub-flavoured tables, alerts (`> [!NOTE]`), task lists, and strikethrough
render as on GitHub, and YAML front matter is kept out of the rendered view.
Raw HTML is escaped, comments outside code are hidden, and relative document
links retain the selected branch or tag. Directory links also work without a
trailing slash. Images, including SVG, load through an immutable protected raw
route. Source views have line numbers, highlighted line links, optional
wrapping, and server-side syntax highlighting for C/C++, Python, shell, YAML,
and JSON (fenced code blocks in Markdown get the same), with no JavaScript. On narrow screens, file navigation is collapsed behind Show files,
and project/commit rows rearrange for reading. Empty projects explain how to
start; missing files retain the project, revision picker, and parent links.
Text and README source previews are limited to 512 KiB. The file preview
embeds images up to 4 MiB; README image references use the raw route's 16 MiB
limit, which also bounds downloads. Files beyond these limits remain available
through Git. The sidebar preloads trees up to 5000 entries and 32 path
components in one bounded Git read. Larger trees fall back to loading the
current directory and its ancestors; opening another folder loads its contents.
These expanded listings retain a combined 5000-entry limit. A rendered page is
limited to 8 MiB of HTML.

Commits and Graph show 50 commits per page with a server-rendered SVG graph.
Lane lines span each row and remain connected when commit text wraps.
Older links retain the selected revision and preserve merged histories; locating
a cursor scans a bounded traversal, so deep paging costs more than a single
page of work. Commit pages include the message, parent links, file statistics,
and a diff capped at 512 KiB. Calendar views show UTC commit activity, and day
pages list up to 200 commits with a link to the tree at the end of that day.
Activity and cursor traversal are bounded to 200000 commits. Limit notices
identify abbreviated output; time and output failures return a bounded error.

Every page works without JavaScript and shares one layout with inline CSS.
HTTP requests perform no writes. There is no persistent index or SQLite
database; standard bare Git repositories remain the source of truth.

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

## Backup and recovery

For verified backups and recovery, see
[Backup and recovery](docs/operations/03-backup-and-recovery.md).
`ckgit-admin backup` preserves reachable Git history, hosted refs and HEAD,
original Git configuration for review, and private project metadata. It verifies
SHA-256 file inventory and Git objects before publishing a new backup directory.
`restore-backup` verifies and stages the recovery, refuses existing project
collisions, and requires an empty target metadata directory. Restored projects
receive fresh hosting safety settings and the target configuration's hooks.
The `trash list` and `restore-project` commands recover retained deleted Git
repositories; their removed checkout registrations are not recreated. Neither
backup nor recovery includes operating-system configuration, SSH keys,
credentials, reflogs, or unreachable objects. Keep those separately as needed.
Recovery commands support `--repo-root` and `--state-root` instead of `--config`,
plus optional `--hook-directory` for the destination hook configuration.

## Install as a service

[docs/operations/01-installation.md](docs/operations/01-installation.md)
describes the packaged layout, the hardened systemd unit, the sshd drop-in that
moves the `ckgit` account's authorized keys to a root-owned file, device
pairing with `ckgit-admin authorized-key`, upgrade, and removal. In short:

```text
make BUILD_DIR=/tmp/ck-git-hosting-my-build all
sudo sh packaging/install.sh --build-dir /tmp/ck-git-hosting-my-build --dry-run
sudo sh packaging/install.sh --build-dir /tmp/ck-git-hosting-my-build --yes
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

## License

Licensed under the MIT License, see [LICENSE](LICENSE).
