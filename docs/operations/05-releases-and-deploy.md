# Self-hosted releases and deployment

`ck-git-hosting` ships two independent release channels for the same tag, and
can deploy itself from either one. This guide covers the second: building,
publishing, and installing a release entirely on your own hardware, with no
GitHub account or outbound internet access required.

## Two channels, one version rule

| | GitHub | Self-hosted |
|---|---|---|
| Trigger | push `vMAJOR.MINOR.PATCH` to GitHub | push the same tag to a CI-enabled project on this server |
| Built by | `.github/workflows/release.yml` | `.ckgit/ci.yml`, run by this server's own `ck-ci-runnerd` |
| Where it lands | a GitHub Release, plus the Homebrew tap formula | the release store under this server's private state root |
| Installed with | `apt install` a downloaded `.deb`, or `brew install` | `ck-git-hosting-deploy` (server), `packaging/update-cli.sh` (client) |

Both channels use the same version rule (see
[Packages, continuous integration, and releases](02-packages-and-releases.md)):
a tag `vMAJOR.MINOR.PATCH` matching the `VERSION` file produces the clean
release version, for example `0.1.0`, on both channels at once, so `v0.1.0`
never means two different things. A branch build (`master`, not a tag) is
always a development snapshot, versioned `VERSION~YYYYMMDD.HHMM.commit`; `~`
sorts below the plain release in dpkg's ordering, so installing a real release
is never mistaken for a downgrade from an earlier snapshot.

Nothing requires using both channels. A server with no GitHub remote and no
internet access can still build, publish, and deploy its own releases purely
through the self-hosted channel described below; that is precisely the
"generic, not hardcoded to one operator's setup" path this guide documents.

## The self-hosted flow, step by step

1. **Enable CI** for the project whose releases you want to deploy (this
   project deploys itself, so the project is `ck-git-hosting`):

   ```text
   sudo ckgit-admin ci enable ck-git-hosting --config /etc/ck-git-hosting/server.ini
   ```

2. **Commit a workflow** at `.ckgit/ci.yml` with a job that declares
   `artifacts: { name: packages, paths: [...] }` (never `name: release` --
   that name is reserved for the release record itself) and builds whatever
   the deployed package needs. See the accepted format in
   [Continuous integration](04-ci-cd.md), and this project's own
   `.ckgit/ci.yml` for a complete, working example: it builds, runs
   `make check`, packages `.deb`s for both architectures plus a source
   tarball, and fails the whole build if a tag does not match `VERSION` -- so
   a mismatched tag can never publish a wrong release.

3. **Push a matching tag** to this server's remote for that project:

   ```text
   git tag -a v0.1.0 -m 'Release 0.1.0'
   git push ckgit v0.1.0
   ```

   The tag build runs like any other CI run (watch it with
   `ckgit-admin ci runs ck-git-hosting --config ...` or the dashboard's CI
   tab), and on success its declared artifact becomes a durable release under
   `<state_root>/releases/ck-git-hosting/v0.1.0/` -- kept until the tag is
   deleted, never swept by the ordinary artifact-retention sweep. A failed tag
   build publishes nothing.

4. **Preview the deploy**, then run it for real:

   ```text
   sudo ck-git-hosting-deploy --dry-run   # selects the newest release, verifies its checksum, prints the plan
   sudo ck-git-hosting-deploy             # installs it
   ```

   `--dry-run` is the only mode that does not require root; it selects a
   release (the newest by default, or `--tag TAG`), verifies the chosen
   asset's sha256 against its own sidecar record before printing anything,
   and changes nothing. The real run installs the `.deb` with
   `dpkg -i --force-confold` (see [Upgrade notes](#upgrade-notes) below),
   restarts `ck-git-hosting`, `ck-ci-runner`, and -- when `pages_http_port` is
   set -- `ck-pages`, clearing each unit's failed-start count first so a
   deliberately broken previous deploy can never block this one via
   systemd's `StartLimitBurst`.

5. **The health check and automatic rollback.** After restarting the
   services, the command polls for up to 45 seconds for every restarted
   service to be simultaneously: active in systemd, answering HTTP 200 on its
   configured port, and reporting the *newly deployed* commit in its runtime
   record -- not just "started", but "started and running the release that
   was just installed". On success it prints the deployed tag and commit and
   exits 0. On failure it reinstalls the last package that was previously
   confirmed healthy (kept under `--deploy-dir`, default
   `/var/lib/ck-git-hosting/deploy`) and re-runs the same health check against
   that package's own commit, so a rollback is verified exactly as strictly
   as a forward deploy -- never just assumed to have worked. `--no-rollback`
   leaves the failed install in place for inspection instead. `--wait
   SECONDS` polls for a currently running CI build to finish rather than
   refusing immediately, since the deploy and the runner share the same
   sandboxed build machinery and must not overlap. `--if-new` exits 0
   quietly when the selected release is already the one deployed, which is
   what an unattended periodic check would use to stay silent on every run
   that has nothing to do. There is no such timer today -- this command runs
   only when an administrator runs it by hand (see
   [The trust boundary](#the-trust-boundary) for why that is deliberate).

6. **Verify** the running version independently of the deploy command's own
   report:

   ```text
   ckgit version
   ```

   This prints the local CLI's build plus, over the same restricted SSH
   control channel as every other `ckgit` command, the versions the server's
   four components report themselves running -- so a stale service that
   failed to restart (rather than failing its health check) is still visible.

Every step above is driven by `.ckgit/ci.yml`, `server.ini`, and command-line
flags -- nothing in `ck-git-hosting-deploy` hardcodes a project name, a host,
or a path beyond its own documented defaults (`--project`, `--asset`,
`--config`, and `--deploy-dir` override every one of them), so the same
command deploys any project's own release the same way.

## The trust boundary

Whoever can push a matching tag to the CI-enabled project you deploy from can
put an arbitrary package in front of root the next time `ck-git-hosting-deploy`
runs: there is no per-ref or per-device access control beyond ordinary Git
push access to that one project. This is a deliberate, contained trade-off,
not an oversight -- the same trust already implied by giving someone push
access to a project whose CI output you install as root. Keep the deploy
command a manual, human-run step (its default state), or only enable the
timer below for a project whose pushers you would already trust with root on
this machine. Do not point it at a project with a wider or less trusted set
of pushers than that.

## Automatic deployment

`ck-git-hosting-deploy.service` and `ck-git-hosting-deploy.timer` are
installed by both the package and the tarball installer, alongside the other
units, but **never enabled by either** -- opting in is a separate, deliberate
step:

```text
sudo systemctl enable --now ck-git-hosting-deploy.timer
```

Once enabled, the timer runs `ck-git-hosting-deploy --if-new --wait 600
--config /etc/ck-git-hosting/server.ini` five minutes after boot and every
ten minutes after the previous run finishes (`OnUnitActiveSec` counts from
completion, not from the previous start, so a slow deploy never causes
back-to-back runs). `--if-new` makes a run with nothing new to deploy exit
`0` quietly, so the steady-state case produces no journal noise; `--wait 600`
defers to a CI build already in progress for up to ten minutes rather than
refusing outright. A missed run (the machine was off) is not made up
retroactively (`Persistent=false`) -- the next regular tick is soon enough
for unattended maintenance, and a catch-up run at boot could otherwise fire
before the network is even up.

Every run's outcome is in the journal:

```text
journalctl -u ck-git-hosting-deploy.service
```

A run that finds nothing to deploy yet (no release has been published for
the configured project) exits non-zero and shows as a **failed** unit --
this is deliberate, not a bug: it surfaces a real misconfiguration (the timer
was enabled before the project ever produced a release) the same way any
other unexpected failure would, rather than swallowing it silently. A
genuinely failed deploy still rolls back automatically, exactly as it would
run by hand; the timer changes only when `ck-git-hosting-deploy` runs, never
how it behaves once it does.

Disabling the timer (`sudo systemctl disable --now
ck-git-hosting-deploy.timer`) returns to the fully manual default without
uninstalling anything. Uninstalling (`packaging/uninstall.sh`, or removing
the package) disables and removes both units unconditionally.

## Deploying your own project

Everything above deploys `ck-git-hosting` itself. To deploy *your* project's
own tag build the same way -- verified, then handed to a command you write --
add a **release hook**: a config file under `/etc/ck-git-hosting/deploy.d/`
naming your project, which releases to act on, and a command to run against
each one. `ck-git-hosting-deploy --all` runs every configured hook; it is the
second of the two commands the deploy timer already runs each cycle (see
[Automatic deployment](#automatic-deployment) above), so enabling that one
timer covers both ck-git-hosting's own deploy and every hook you configure.

Copy the shipped template and edit it for your project:

```text
sudo cp /etc/ck-git-hosting/deploy.d/ck-git-hosting.conf.example \
        /etc/ck-git-hosting/deploy.d/myapp.conf
sudo chown root:root /etc/ck-git-hosting/deploy.d/myapp.conf
sudo chmod 0600 /etc/ck-git-hosting/deploy.d/myapp.conf
sudo "$EDITOR" /etc/ck-git-hosting/deploy.d/myapp.conf
```

A hook config is `key=value`, one per line:

| Key | Required | Meaning |
|---|---|---|
| `project` | yes | the hosted project whose releases to watch |
| `asset` | yes | the release asset (`artifacts: name` in its `.ckgit/ci.yml`) to fetch |
| `tags` | yes | space-separated tag patterns to act on -- the same exact-name-or-trailing-`*` syntax as `.ckgit/ci.yml`'s own `on.tags` |
| `command` | yes | an absolute path to the command to run |
| `services` | no | space-separated systemd unit basenames to health-check (active, within about 15 seconds) after `command` runs |
| `health_url` | no | a URL that must answer HTTP 200 after `command` runs |
| `timeout_seconds` | no | wall-clock budget for `command` itself (default 300) |

For each hook, `--all` selects the newest release of `project` whose tag
matches one of the `tags` patterns and is not already recorded as run for
that hook, verifies its asset's checksum from its own sidecar exactly as the
built-in deploy does, extracts it, and runs:

```text
command EXTRACTED-DIR TAG PROJECT
```

**There is no built-in rollback for a hook** -- `command` owns that,
the same way it owns everything else about what "deployed" means for your
project (installing a binary, restarting a service, writing files
somewhere, or nothing filesystem-related at all). `services`/`health_url`
are optional conveniences that only report a problem; `--all` never retries
or reverts on your behalf. A hook runs at most once per release, success or
not; an administrator who wants to force a retry removes
`/var/lib/ck-git-hosting/deploy/<name>/deployed-tag` (`<name>` is the config's
own filename without `.conf`) by hand.

**The security boundary is stricter here than the trust boundary above
already implies**, because a hook config *names an arbitrary command* rather
than always running the one audited `dpkg -i`: `/etc/ck-git-hosting/deploy.d/`
itself, every `*.conf` file read from it, and every file a `command=` points
at must be owned by root and have no permission bits set for group or other
(`0700`/`0600`, or tighter) -- checked fresh before every run, not just once.
A directory, config, or command that fails this check is never read or run;
the whole `--all` pass refuses outright if the directory itself fails it,
and an individual misconfigured hook is skipped (with the reason logged) so
one bad config does not stop every other hook's project from deploying.
This means only root -- not the `ckgit` service account, not any other user
-- can ever define what `--all` runs, which is exactly the same authority
installing your own hook already requires.

`ck-git-hosting-deploy.service`'s own two-step design (the built-in deploy,
then `--all`) means the second step does not run in a cycle where the first
one fails -- see that unit's header comment. Run `sudo ck-git-hosting-deploy
--all --dry-run` by hand any time to preview what the next cycle would do
without waiting for the timer or for the built-in deploy to succeed first.

## Upgrade notes

`/etc/ck-git-hosting/server.ini` is a dpkg conffile, and `ck-git-hosting-deploy`
always installs with `dpkg -i --force-confold`: your edited `server.ini`
survives every deploy completely unchanged, including across a rollback. This
also means a release that adds a **new** optional configuration key never adds
it to your existing file automatically -- `--force-confold` keeps the old file
byte for byte, new key and all its documentation included. To pick up a new
feature that a release introduces through a new key, add that key to
`server.ini` yourself (see the key tables in
[Configure the runner](04-ci-cd.md#configure-the-runner) and
[installation](01-installation.md)) and restart the affected service.

No release has shipped yet as of this writing, so there is no key-addition
history to list here. Once releases exist, each one that adds a new
`server.ini` key will be noted here by version, so upgrading past it is a
one-line check rather than a full config diff.

## Updating the client

`packaging/update-cli.sh` updates a device's own `ckgit` from this project's
own self-hosted release, using `ckgit release download` (see
[Releases](04-ci-cd.md#releases)) -- no SSH login or sudo access on the server
at all, only an existing `ckgit` already paired through `client.ini`:

```text
sh packaging/update-cli.sh                       # newest release -> /usr/local/bin
sh packaging/update-cli.sh --tag v0.1.0 --prefix ~/bin
```

It downloads the release's `packages` asset, unpacks the source tarball inside
it, builds just the client (`make ... client`) stamped with the exact version
baked into that release, and installs the result -- so a laptop or workstation
that cannot run the server's Linux binaries directly still ends up on the
exact CLI version the server was upgraded to.
