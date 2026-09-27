# Follow builds and download releases

## Follow a build

```sh
ckgit ci list --project my-project
ckgit ci watch --project my-project
```

`ckgit ci` follows a project's CI from any paired device over the same
restricted SSH control channel. Inside a checkout it selects the paired
project; `--project NAME` selects another. `ci status` shows whether CI is
enabled with the latest and active runs (`--all` for every project), `ci list`
the newest runs, `ci show RUN` one run's steps and artifacts, `ci log RUN`
a step's raw log (`--follow` tails the running step and the ones after it),
and `ci watch` waits for a run and exits 0 on success or 5 when it finished
otherwise. `ci artifacts` and `ci download` list and fetch a run's bundles
exactly like `release list` and `release download`. `ci cancel RUN` previews
the run and asks before it requests a stop (`--dry-run`, `--yes`), and
`ci lint` checks `.ckgit/ci.yml` offline with the runner's own parser.
Enabling or disabling CI, rerunning or triggering runs, and secrets stay
administrator-only (`sudo -u ckgit ckgit-admin ci ...` on the server). See
[Watch and cancel a run](watch-ci.md#watch-and-cancel-a-run).

## Download a release

```sh
ckgit release list my-project
ckgit release download my-project --tag v1.0.0 --into downloads
```

`ckgit release list PROJECT` shows that project's durable, tag-triggered
releases (tag, commit, creation time, and each asset's name, size, and
checksum) over the same restricted SSH control channel as `refs` and
`refresh` -- no dashboard tunnel needed. `ckgit release download PROJECT`
lists the same way, picks the newest release or `--tag`, picks its only asset
or `--asset`, then opens a tunnel exactly like `web` (the ordinary SSH login,
since the restricted `ckgit` account still allows no forwarding) to fetch it,
verifies its size and checksum against the listing, and writes
`--into DIR/NAME.tar` (default: the current directory) only once that
verification passes. See [Releases](../operations/releases.md#releases) for
how a release is produced and `packaging/update-cli.sh` for a scripted
example that updates this same CLI from one.
