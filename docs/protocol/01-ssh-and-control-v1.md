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
ckgit-rpc 1 ping
ckgit-rpc 1 list-projects
ckgit-rpc 1 refs project
ckgit-rpc 1 create project branch
ckgit-rpc 1 register project path-hex
```

`project` must meet the first-release project-name grammar.  The quoted
repository argument is a single literal token, must be exactly `project.git`,
and cannot contain a slash, backslash, option prefix, or traversal component.
The dispatcher derives the filesystem path below its configured repository root
and verifies that it is an existing, non-symlink directory before directly
executing the fixed Git service program.

All other commands, arguments, protocol versions, malformed quoting, controls,
and commands longer than 1024 bytes fail closed.  `ckgit-rpc` is a deliberately
separate command family: it never becomes an argument to Git.

## Receive-hook identity handoff

When the dispatcher is configured with a private metadata state root, it starts
the fixed `git-receive-pack` program with only `PATH`, `LANG`, and these three
additional variables: `CKGIT_CLIENT_ID`, `CKGIT_PROJECT_NAME`, and
`CKGIT_STATE_ROOT`. The values are derived from the forced command's fixed
client ID, the accepted repository token, and the daemon-owned command-line
configuration; none comes from `SSH_ORIGINAL_COMMAND` after parsing.

The installed compiled `post-receive` hook accepts only bounded standard Git
`old-id new-id refname` records. It rejects malformed, oversized, or unexpected
ref namespaces and appends one `git-push` event without storing a ref name,
checkout path, or remote URL. A local administrative receive without all three
variables completes normally but has no authenticated client event to record.

## Control socket framing

The dispatcher connects only to its configured Unix-domain socket.  It sends one
bounded UTF-8 ASCII request and then half-closes its write side:

```text
CKGIT-CONTROL/1 <client-id> <operation>\n
```

For version 1, `operation` is `ping`, `list-projects`, `refs`, `create`, or
`register`. `refs` adds a single validated project-name argument. `create` adds
a validated project name and branch. `register` adds a project and a lowercase
hex encoding of the client-selected display path: decoded data must be
printable, valid UTF-8, and at most 256 bytes. The daemon checks the Unix peer
credentials as an independent boundary; it must not trust the client ID merely
because it appears in this line.

The daemon sends at most one 4096-byte response.  Its first line is one of:

```text
ok\n
ok <safe-metadata>\n
error <stable-code> <safe-message>\n
```

An `ok` response may have newline-delimited ASCII payload lines after the first
line.  `list-projects` uses `ok <count>` followed by one project name per line.
`refs` uses `ok <count>` followed by `refname object-id` lines.  No response
contains a secret, checkout path, or raw remote URL. `create` responds with
`ok created` only after the bare repository and receive settings are in place.
`register` responds with `ok registered` only after a staged, fsynced, atomic
record has replaced that client/project's prior registration in the configured
private state root.

Unknown protocol versions and operations receive a bounded `error` response.
Future operations that accept structured metadata will use a separately
versioned length-delimited payload; they must not extend this line grammar.

## Compatibility rule

An unknown major protocol version is rejected.  A future compatible operation
may be added only after its exact request grammar, size limits, authorization,
and negative tests are documented here.
