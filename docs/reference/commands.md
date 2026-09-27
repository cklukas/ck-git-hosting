# Command-line reference

For guided examples, start with [client setup](../guides/client-setup.md) or [publishing a project](../guides/publish-and-sync.md).

## Commands

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
ckgit ci status [--project NAME | --all] [--json]
ckgit ci list [--project NAME] [--limit N] [--json]
ckgit ci show RUN [--project NAME] [--json]
ckgit ci log RUN [STEP] [--project NAME] [--follow]
ckgit ci watch [RUN] [--project NAME] [--interval SECONDS]
ckgit ci artifacts RUN [--project NAME] [--json]
ckgit ci download RUN NAME [--project NAME] [--into DIR]
ckgit ci cancel RUN [--project NAME] [--dry-run] [--yes]
ckgit ci lint [PATH] [--rev REV]
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
ckdocs serve [--root DIR] [--out DIR] [--port PORT] [--quiet]
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
