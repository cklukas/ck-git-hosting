# SSH and control protocol version 1

This document defines the first restricted-command boundary.  It is intentionally
small enough to reject before any repository path is opened or process is
started.  The protocol version is independent from the product release version.

## SSH forced command

Each client key is paired with a fixed command similar to:

```text
restrict,command="/usr/bin/ck-git-shell --client-id mac-studio --repo-root /srv/ck-git-hosting/repos --control-socket /run/ck-git-hosting/control.sock" ssh-ed25519 AAAA... mac-studio
```

The dispatcher reads `SSH_ORIGINAL_COMMAND`; it does not invoke a shell.  Version
1 accepts only the following complete command forms:

```text
git-upload-pack 'project.git'
git-receive-pack 'project.git'
git-upload-pack './-project.git'
git-receive-pack './-project.git'
ckgit-rpc 1 ping
ckgit-rpc 1 version
ckgit-rpc 1 versions
ckgit-rpc 1 list-projects
ckgit-rpc 1 checkouts
ckgit-rpc 1 refs project
ckgit-rpc 1 refresh project
ckgit-rpc 1 create project branch
ckgit-rpc 1 default-branch project
ckgit-rpc 1 set-default-branch project branch
ckgit-rpc 1 register project path-hex
ckgit-rpc 1 replace-checkout project path-hex
ckgit-rpc 1 forget-checkout project
ckgit-rpc 1 releases project
ckgit-rpc 1 ci-overview
ckgit-rpc 1 ci-status project
ckgit-rpc 1 ci-run project run-id
ckgit-rpc 1 ci-placement project run-id
ckgit-rpc 1 ci-log project run-id step offset
ckgit-rpc 1 ci-cancel project run-id
```

`project` must meet the first-release project-name grammar.  The quoted
repository argument is a single literal token, must be exactly `project.git`,
and cannot contain a slash, backslash, or traversal component. The sole path
prefix exception is `./` immediately before a project name beginning with `-`:
Git itself rejects a transport path beginning with `-`, so clients use
`user@host:./-project.git`. The dispatcher strips exactly that prefix and then
validates the complete project name. Other relative prefixes and nested paths
remain rejected.
The dispatcher derives the filesystem path below its configured repository root
and verifies that it is an existing, non-symlink directory before directly
executing the fixed Git service program.

Every `ckgit-rpc` token after the command name is unquoted and uses only
`[A-Za-z0-9._-]`. `run-id` is a CI run id as the store generates it: 1 to 64
characters of `[A-Za-z0-9-]`. `step` is a canonical decimal step index from
`0` to `4096` (the most results one run record holds; the running step's
index equals the number of completed steps), and `offset` a canonical decimal
byte offset that fits 64 bits. Canonical means digits only, no sign, and no
leading zero unless the value is `0`, so every number has one spelling.
A command therefore has at most seven tokens: `ckgit-rpc`, the version, the
operation, and at most four arguments; only `ci-log` uses all four.

All other commands, arguments, protocol versions, malformed quoting, controls,
and commands longer than 1024 bytes fail closed.  `ckgit-rpc` is a deliberately
separate command family: it never becomes an argument to Git.

## Receive-hook identity handoff

The dispatcher starts the fixed `git-receive-pack` program with a restricted
environment: `PATH`, `LANG`, `CKGIT_CLIENT_ID`, `CKGIT_PROJECT_NAME`,
`CKGIT_CONTROL_SOCKET`, and `CKGIT_REPOSITORY_ROOT`, plus `CKGIT_STATE_ROOT`
when configured and `TMPDIR` when supplied by the launch environment.
Identity, project, repository/state roots, and socket
values are derived from the forced command's fixed client ID, the accepted
repository token, and daemon-owned command-line configuration; none is copied
from unparsed `SSH_ORIGINAL_COMMAND` text.

The installed compiled `post-receive` hook accepts only bounded standard Git
`old-id new-id refname` records: at most 200000 updates and 16 MiB of input,
enough for the first push of any real history. It rejects malformed,
oversized, or unexpected ref namespaces and appends one `git-push` event
without storing a ref name, checkout path, or remote URL when a state root is
configured. After a nonempty accepted update, it sends
`CKGIT-CONTROL/1 <client-id> refresh <project>` to the configured control socket.
Connect, send, and response reads share one two-second deadline. A refresh
transport error or rejected request is ignored, so an unavailable dashboard
does not fail the Git push. Notification also works when no state root is
configured; if event logging fails, the hook still attempts the notification
before reporting the logging error. A local administrative receive without
client/project identity records no authenticated event and relies on the
periodic fingerprint sweep. Partially supplied or invalid identity is rejected.
The configured repository root lets the hook recheck repository existence
under the lifecycle lock before appending an event: a push finishing after
administrative removal cannot recreate that project's event metadata.

## Control socket framing

The dispatcher connects only to its configured Unix-domain socket.  It sends one
bounded UTF-8 ASCII request and then half-closes its write side:

```text
CKGIT-CONTROL/1 <client-id> <operation> [<argument> ...]\n
```

The request line is at most 768 bytes including its final newline and holds
at most six space-separated tokens after the prefix: the client ID, the
operation, and at most four arguments.
For version 1, `operation` is `ping`, `version`, `versions`, `list-projects`,
`checkouts`, `refs`, `refresh`, `create`, `default-branch`,
`set-default-branch`, `register`, `replace-checkout`, `forget-checkout`,
`releases`, `ci-overview`, `ci-status`, `ci-run`, `ci-placement`, `ci-log`, or
`ci-cancel`,
each with exactly the arguments the SSH forms above
show. `ping`, `version`, `versions`, `list-projects`, `checkouts`, and
`ci-overview` take none.
`refs`, `refresh`, `forget-checkout`, `releases`, `ci-status`, and
`default-branch` add one validated project-name argument and reject any
further arguments. `create` and `set-default-branch` add a validated project
name and branch.
`register` and `replace-checkout` add a project and a lowercase hex encoding of
the client-selected display path: decoded data must be printable, valid UTF-8,
and at most 256 bytes. The daemon checks the Unix peer credentials as an
independent boundary; it must not trust the client ID merely because it
appears in this line.

The SSH dispatcher, the forwarding client in `ck-git-shell`, the daemon, and
the `ckgit` client that composes requests all check an operation against one
shared operation table, so the boundaries cannot drift apart; each still
applies its own framing checks (quoting and the 1024-byte command at the SSH
boundary, the 768-byte printable line and client ID at the daemon). The daemon
answers every operation from its own branch and refuses an operation it does
not implement with `error request`, never with another operation's reply.

A host keeps one main checkout per project. Registration requires an existing,
non-symlink hosted repository; its existence check, metadata write, and event
append share the repository lifecycle lock with removal and orphan cleanup.
`register` creates a registration or refreshes it when the path is unchanged;
when the same client already
registered a different path it fails with `error conflict ...` and changes
nothing. `replace-checkout` performs the same write unconditionally and is the
explicit way to move a project's main checkout on that host. Two reports name
the same checkout when they are equal, or when one is a bare folder name (the
client's basename privacy mode) matching the final component of the other; a
refresh that reports only the name keeps the previously stored full path. The
client additionally compares a stored full path exactly against its own real
path, so the name-only equivalence never hides a different folder from the
host that owns it.

`forget-checkout` idempotently removes only the authenticated client's
registration for the named project under the repository lifecycle and metadata
locks. It accepts exactly one project argument; the caller cannot supply a
different client ID or a filesystem path. Missing projects and records are
allowed, so stale registrations can be cleaned without recreating metadata.
It never changes the hosted repository, other clients' records, or checkout
files. Success is exactly `ok forgotten`. The client removes its private local
selection only after receiving that acknowledgement. Negative tests cover
extra arguments, unsafe names, symlinks, and cross-device isolation.

`releases` accepts exactly one project argument and lists that project's
durable, tag-triggered releases, each produced by a `.ckgit/ci.yml` `package`
step run against a pushed tag. It
takes no authorization beyond the standard project-name argument shape: the
listing carries only a release's tag, commit, creation time, and its assets'
names, sizes, and checksums, which is no more than a client already learns
from `refs`, so it is available to any client that can reach the control
socket. It never touches the hosted repository or another client's records.
Negative tests cover extra arguments, an unsafe or unknown project name, and
a project with no releases (`ok 0`, no further lines).

### Default branch operations

`default-branch` and `set-default-branch` serve `ckgit default-branch`. A
project's default branch is the branch its bare repository's `HEAD` names.

```text
default-branch:
ok <branch> <present|unborn>

set-default-branch:
ok changed <previous-branch|->
ok unchanged
```

- `default-branch project` reports the branch `HEAD` names and whether it has
  a commit yet: `unborn` for a project nothing was pushed to. A `HEAD` that
  names no valid branch is answered with
  `error nohead the project's HEAD does not name a branch`.
- `set-default-branch project branch` points `HEAD` at `refs/heads/<branch>`
  with `git symbolic-ref`, which replaces it atomically through Git's lock
  file. The daemon holds the repository lifecycle lock shared with creation
  and removal, and refuses a branch that does not exist or has no commit with
  `error nobranch no such branch on the server`: `HEAD` never names a missing
  branch. Hosted repositories deny ref deletion, so the verified branch cannot
  disappear before `HEAD` is moved. Selecting the current default branch is
  answered `ok unchanged` and changes nothing. A change answers
  `ok changed` with the branch `HEAD` named before (`-` when it named none),
  appends a `default-branch-changed` event naming the requesting client ID,
  and queues an index refresh so the dashboard shows the new default branch.

Both operations answer an unknown project with
`error noproject no such project`. The branch argument has the same grammar
as `create`'s; because every `ckgit-rpc` token is limited to
`[A-Za-z0-9._-]`, a branch name containing `/` cannot be selected over SSH
and is set on the server with `ckgit-admin default-branch` instead.

Authorization: any authorized device may read or change any project's
default branch, under the same same-user peer check as every operation.
`default-branch` returns only what `refs` and a clone already reveal. The
change is acceptable for every key for the same reason as `ci-cancel`: it is
weaker than push, which every key already holds and which can rewrite what the
default branch contains; it destroys no ref or commit, is attributed in the
event log, and is undone by the same command.

Negative tests cover, at the SSH dispatcher, the daemon's request parser, and
the forwarding client: a missing or extra argument, an unsafe branch or
project, and a branch with a leading `-`. The daemon's replies are covered for
an unknown project, a missing branch, an unborn default branch, and an
unchanged selection, and end to end by `ckgit default-branch` and
`ckgit-admin default-branch`.

### CI operations

The six `ci-*` operations serve `ckgit ci` on a paired device: read-only
inspection of runs, and cancellation. Enabling or disabling CI for a
project, rerunning or triggering a run, and CI secrets are deliberately not
control operations: they stay administrator-only, through `ckgit-admin` on
the server. Every CI operation requires the configured state root and, apart
from `ci-overview`, a hosted project: an unknown project is answered with
`error noproject no such project` and an unknown run with
`error norun no such CI run`, so a mistyped name is never reported as an
empty history. Free text -- a ref, a run's detail line, a step name, an
artifact note, log bytes -- travels as lowercase hex (`-` for empty text), and
every header carries the server's clock as `<server-epoch>` so a client
judges a stale heartbeat by server time, not its own. Counts come first and
each parser rejects a response whose records do not match them, whose run ids
are out of order or repeated, or whose fields fail the same validation the
store applies.

```text
ci-overview / ci-status:
ok <server-epoch> <project-count>
project <name> <enabled|disabled> <run-count>
run <run-id> <status> <started> <finished> <heartbeat> <completed-steps> <commit> <ref-hex>

ci-run:
ok <server-epoch> <artifact-count>
run <run-id> <status> <started> <finished> <heartbeat> <completed-steps> <commit> <ref-hex>
detail <detail-hex>
step <index> <exit-code> <timed-out 0|1> <truncated 0|1> <name-hex>
artifact <name> <bytes> <sha256|-> <created> <expires> <note-hex>

ci-placement:
ok <runner|-> <attempt>

ci-log:
ok <more|end> <offset> <byte-count>
<hex bytes>
```

- `ci-status project` lists the project's opt-in and its 64 newest runs,
  newest first, each as one `run` line. `status` is the recorded status
  (`pending`, `running`, `success`, `failure`, `timeout`, `error`,
  `skipped`, `cancelled`); `completed-steps` is also the running step's index.
- `ci-overview` lists every hosted project, sorted by name, with its newest
  run and, when that run is not pending or running, the newest one that is,
  looked for among the project's 16 newest runs. A listing that would exceed
  the response limit is refused with `error internal` rather than truncated;
  at a few hundred bytes per project that is several hundred projects.
- `ci-run project run-id` returns one run: its `run` line, a `detail` line,
  exactly `completed-steps` `step` lines in index order, and `artifact-count`
  `artifact` lines (`expires` is `0` for a durable bundle; a note means the
  bundle was not stored). A record too large for the response limit -- only
  possible with thousands of steps -- is refused with `error internal`.
- `ci-run` also folds the run's placement into its `detail` line, so clients
  without `ci-placement` still show it: ` (on <runner>, attempt <n>)` is
  appended (or the note alone for an empty detail) when the runner is not
  `primary` or the attempt is above 1, and the result is cut at the detail
  limit.
- `ci-placement project run-id` returns which runner executes or executed
  the run -- `primary` for the server's own runner, a remote runner's name
  (the client-ID grammar) otherwise, `-` while the run is queued -- and its
  attempt, a canonical decimal of at least 1. A client that knows both strips
  exactly the note `ci-run` appended. It reads only the run record, like
  `ci-run`, and answers `noproject` and `norun` the same way; a server older
  than this operation refuses it at the SSH dispatcher, and clients then fall
  back to the folded detail.
- `ci-log project run-id step offset` returns up to 114688 (112 KiB) bytes of
  that step's log from `offset`, hex-encoded on one line (absent when
  `byte-count` is 0), so the reply stays under the response limit. The daemon
  reads the run record before the log: the runner writes a step's whole log
  before recording its result. `end` means no further bytes will follow -- the
  step has a recorded result, or the run is no longer live (finished, or
  interrupted because its heartbeat is more than 90 seconds old), and this
  chunk reaches the end of the file. `more` means ask again from
  `offset + byte-count`; for the step currently running (or a later step of a
  live run) that may be an empty chunk. A step without a log in a run that
  can no longer write it is answered with `error nolog`.
- `ci-cancel project run-id` writes the run's cooperative cancel marker, the
  same marker the dashboard's Cancel button and `ckgit-admin ci cancel` drop,
  and answers `ok cancelling`; the runner kills the current step at its next
  check and records the run `cancelled`. A run that already finished is left
  unchanged and answered with `error finished the CI run already finished`.
  The daemon appends a `ci-cancel-requested` event naming the requesting
  client ID, which the project page lists under recent events.

Authorization: the daemon authorizes peers only by same-user credentials, and
every device key reaches every project with the same rights. The read-only
CI operations need nothing beyond their argument shape because a run record
holds only what a device with read access already has or builds from
repository content: refs, commits, step names and output from the committed
workflow, and artifact metadata -- the same data the dashboard shows. None
touches the hosted repository, another client's records, or a filesystem
path the request could steer: the project and run id are validated tokens
the store resolves below its own private root. `ci-cancel` is the one
mutation: any authorized device may cancel any project's pending or running
run. That is acceptable because it is strictly weaker than push, which every
key already holds -- a push can replace the commit a run builds, or queue new
runs at will -- and a cancel destroys nothing: it stops one build, which is
recorded as cancelled, attributed in the event log, and repeated by pushing
again.

Negative tests cover, at the SSH dispatcher, the daemon's request parser, and
the forwarding client: every CI operation with a missing, extra, or unsafe
argument; a step index above 4096 or with a leading zero; an offset beyond 64
bits; a quoted token; the unlisted `cancel` spelling; and administrator
actions such as `ci-enable`. The daemon's replies are covered for unknown
projects and runs, a project without runs (`project alpha disabled 0`), a
finished run's cancel, and end to end by `ckgit ci` against real runs. The
client parsers reject count mismatches, reordered or repeated records,
malformed hex, non-canonical numbers, and chunks for a different offset.

The daemon sends at most one response of 256 KiB (262144 bytes), enough for
a listing of tens of thousands of refs while still bounding every client
buffer.  Its first line is one of:

```text
ok\n
ok <safe-metadata>\n
error <stable-code> <safe-message>\n
```

An `ok` response may have newline-delimited ASCII payload lines after the first
line.  `version` responds with `ok <build-identifier>`, the same identifier
`ckgit --version` and the dashboard's About panel report for this build; it
takes no arguments and changes nothing. `versions` responds with
`ok versions` followed by one `name version running|stopped started-epoch`
line per suite service that has recorded itself under the state root (the
hosting daemon always reports its own in-process build first); `ckgit version`
shows them beside the local build. It takes no arguments, changes nothing,
and needs no authorization beyond the transport: the lines carry only
component names, build identifiers, and start times.
`list-projects` uses `ok <count>` followed by one project name per line.
`refs` uses `ok <count>` followed by `refname object-id` lines; a client
rejects a count above 65536 or a listing that does not match it. `releases`
uses `ok <count>` where `<count>` is the number of releases, each contributing
one `release tag commit created-epoch` line followed by zero or more
`asset tag name bytes sha256` lines (`sha256` is `-` when the asset predates
checksums), newest release first and bounded to 64 releases; `sha256` and
`bytes` let `ckgit release download` verify an asset before it replaces
anything on disk. `checkouts`
uses `ok <count>` followed by `project path-hex` lines and lists only the
registrations made under the requesting client ID, which the dispatcher fixes
from the forced command; a host can therefore retrieve its own checkout
folders on every machine it owns but never another host's. Apart from that
reply to its own author, no response contains a secret, checkout path, or raw
remote URL. `create` responds with
`ok created` only after the bare repository and receive settings are in place.
`register` responds with `ok registered` only after a staged, fsynced, atomic
record has replaced that client/project's prior registration in the configured
private state root.

`refresh` responds with `ok refreshed` after queueing a deduplicated,
asynchronous index refresh. It uses the same Unix peer-credential check as all
other operations and never creates a repository. The request names a project
even when its repository has just been removed; refreshing that name then
drops the stale index record. An acknowledgement means that work was queued,
not that the new snapshot is already published. Startup indexing and the
60-second sweep remain available when no notification is sent.

Unknown protocol versions and operations receive a bounded `error` response.
Operation arguments are fixed-arity scalar tokens -- names, ids, and canonical
decimals, each validated for its position -- and four is the maximum, reached
only by `ci-log`, whose step and offset make a log readable in bounded chunks.
Raising the limit from two arguments to four for that one operation keeps
every value a separately validated token; packing step and offset into one
argument would instead need a second, private grammar inside a token.
Future operations that accept structured metadata will use a separately
versioned length-delimited payload; they must not extend this line grammar.

## Runner keys

A remote CI runner (see
[Share CI load with remote runners](../operations/ci-remote-runners.md)) uses a
key of its own class. Its forced command adds `--runner-config FILE` (the
server configuration the gateway reads):

```text
restrict,command="/usr/bin/ck-git-shell --client-id uconsole --repo-root /srv/ck-git-hosting/repos --control-socket /run/ck-git-hosting/control.sock --state-root /var/lib/ck-git-hosting/state --runner-config /etc/ck-git-hosting/server.ini" ssh-ed25519 AAAA... uconsole
```

With it, `ck-git-shell` accepts **only** the runner protocol, version 1, and
executes `ck-ci-runnerd gateway` with the validated command line; Git services
and `ckgit-rpc` are refused. A device key (no `--runner-config`) refuses every
runner operation. The client id `primary` is reserved for the server's own
runner.

```text
ckgit-runner 1 hello <version-hex>
ckgit-runner 1 claim <wait-seconds>
ckgit-runner 1 upload-pack <job-id> <token> <project>
ckgit-runner 1 stream <job-id> <token>
ckgit-runner 1 heartbeat <job-id> <token>
ckgit-runner 1 put <job-id> <token> <kind> <name> <offset> <total> <sha256>
ckgit-runner 1 finish <job-id> <token>
ckgit-runner 1 abandon <job-id> <token>
```

Tokens are unquoted `[A-Za-z0-9._-]`; a command is at most 1024 bytes and ten
tokens. `wait-seconds` is a canonical decimal from 0 to 120; `job-id` is a CI
run id; `token` is the 32 lowercase hex characters a claim returned; `kind` is
`artifact`, `asset`, or `pages`; `name` is an artifact name (`site` for
`pages`); `offset` and `total` are canonical decimals with `offset <= total`;
`sha256` is 64 lowercase hex characters.

- `hello` records the runner's build version and last contact and answers
  `ok` with the protocol version.
- `claim` answers `none`, or `job` followed by `key=value` lines: the job, its
  lease token and length, the server's limits, the release tag and
  default-branch flag, each sister resolved to a commit, and the workflow text
  (hex). It long-polls up to `wait-seconds`, woken by the queue's change
  notification. A disabled runner always gets `none`. Jobs that end without
  running (a cancel while queued, no workflow, a ref that is no trigger) are
  finished on the server and never handed out.
- `upload-pack` runs `git-upload-pack` for the claimed job's project or one of
  the sisters its workflow declares, allowing any reachable commit to be
  fetched by hash; it is what a runner's Git fetch talks to.
- `stream` is one long-lived session per run. The server first sends
  `held <step> <bytes>` for each step log it already holds, then `ready`. The
  runner then sends lines `started <epoch>`, `step-start <n> <name-hex>`,
  `log <n> <offset> <length>` followed by exactly `<length>` (at most 65536)
  raw bytes, `step-end <n> <exit> <timed-out> <truncated> <name-hex>`,
  `heartbeat`, and `progress <detail-hex>` (an empty hex field is `-`). The
  server appends a log frame only at the offset its file ends (an overlapping
  frame is stored once; a gap is answered with `held <n> <bytes>`), applies its
  own log cap, keeps the run record current, and answers `ack <n> <bytes>`,
  `cancel` when a cancel was requested, and `lease-lost` when the job was
  requeued.
- `heartbeat` renews the lease outside a stream (during uploads) and answers
  `ok`, `cancel`, or `lease-lost`.
- `put` appends its stdin (at most 4 MiB) to a staged upload at `offset`; the
  first chunk announces `total` and `sha256`, the cap is checked against
  `total` before any byte is stored, and a chunk the server already holds is
  ignored. It answers `ok <bytes-held>`.
- `finish` reads the final record from stdin (`status=`, `started_epoch=`,
  `detail_hex=`, then `step=` and `output=` lines), verifies every staged
  upload, commits artifacts, the release and the Pages site, releases the job,
  and answers `ok <status>`.
- `abandon` hands the job back to the queue at once.

Every operation on a job is refused (`lease-lost`, or `error lease-lost`)
unless the job is claimed by this runner under this token.

## Compatibility rule

An unknown major protocol version is rejected.  A future compatible operation
may be added only after its exact request grammar, size limits, authorization,
and negative tests are documented here.
