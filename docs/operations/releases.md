# Create and download releases

## Releases

Pushing a **tag** turns a build into a durable release — this is where v1
installers live. When a tag build's job declares `artifacts:`, those bundles are
stored under the tag and kept until the tag is deleted; they never expire and the
retention sweep never touches them. A tag triggers a release when it matches
`on: { tags: [...] }`, or always when the workflow omits `on:`.

- The **Releases** tab lists each tag newest-first, using the annotated tag's
  message as the release notes, with every asset's size, checksum, and a
  download link.
- A failed tag build publishes nothing.
- Deleting the tag removes its release and every asset; deleting the project
  removes them too. There is no expiry and no admin step — pushing and deleting
  tags is the whole workflow.

```text
git tag -a v1.0.0 -m 'Release 1.0'      # annotate for release notes
git push origin v1.0.0                   # build and publish the release
git push origin :refs/tags/v1.0.0        # delete the tag -> delete the release
```

Download a release asset the same way as a CI artifact:

```text
curl -O http://<server>:<http_port>/project/myproject/releases/<tag>/<name>
```

Or from any paired device, without `curl` or a known `http_port`:

```text
ckgit release list myproject                       # tag, commit, assets, newest first
ckgit release list myproject --json                 # the same, as a versioned report
ckgit release download myproject --asset NAME        # newest release's asset -> ./NAME.tar
ckgit release download myproject --tag v1.0.0 --asset NAME --into /tmp
```

`release list` queries the same restricted SSH control channel as `refs` and
`refresh`. `release download` lists the same way, then opens a tunnel exactly
like `ckgit web` (the administrator's own SSH login; the restricted `ckgit`
transport account allows no forwarding) to fetch the asset, verifies its size
and checksum against the listing before writing anything, and closes the
tunnel. `--asset` is optional only when the release has exactly one asset.
`packaging/update-cli.sh` is a worked example: it updates a device's own
`ckgit` from this project's own self-hosted release, with no SSH login or
sudo access on the server at all.

### Deploying ck-git-hosting's own release

This project builds and packages itself through its own CI (`.ckgit/ci.yml`
at the repository root): a tag push produces a release whose `packages` asset
is the server `.deb`. The packaged `ck-git-hosting-deploy` command installs
it and restarts the services, with a health check and automatic rollback to
the last healthy package:

```text
sudo ck-git-hosting-deploy --dry-run   # preview the newest release
sudo ck-git-hosting-deploy             # install it
sudo ck-git-hosting-deploy --tag v0.1.1
sudo ck-git-hosting-deploy --help      # every option, including --services,
                                        # --deploy-dir, --wait, --no-rollback
```

It reads `state_root`, `http_port`, and `pages_http_port` from
`/etc/ck-git-hosting/server.ini` (override with `--config`), verifies the
release asset's checksum from its own sidecar before installing anything,
refuses to run while a CI build is in progress (`--wait SECONDS` polls
instead of refusing immediately), and confirms every daemon reports the
release's commit before declaring success — not just that the services
restarted. Nothing here runs on its own; there is no timer yet, so a release
is deployed only when this command is run by hand. See
[Packages, continuous integration, and releases](02-packages-and-releases.md)
for how the packages themselves are built.
