# Packages, continuous integration, and releases

## What is built where

| Artifact | Built by | Target |
|---|---|---|
| `ck-git-hosting_V_arm64.deb`, `ck-git-hosting_V_amd64.deb` | `packaging/build-deb.sh` in a Debian 13 container | Debian 13 servers, including Raspberry Pi 4/5 on arm64 |
| `ckgit_V_arm64.deb`, `ckgit_V_amd64.deb` | same | Debian 13 clients |
| `ck-git-hosting-V-linux-{arm64,amd64}.tar.gz` | `packaging/build-tarball.sh` | other systemd distributions with glibc 2.41 or newer |
| `ckgit-V-macos-{arm64,x86_64}.tar.gz` | same, `--client-only` | macOS clients |
| `ck-git-hosting-V-source.tar.gz` | `git archive` | Homebrew formula source |
| `SHA256SUMS` | release job | verification |

The version comes from the `VERSION` file. A release is created by pushing a
tag `vMAJOR.MINOR.PATCH` that matches it; the workflow refuses a mismatch.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

- Linux amd64 and arm64 with GCC and Clang: strict `-Werror` build plus the
  unit and integration suites (`make all check`).
- macOS Apple silicon and Intel: the same.
- Debian 13 container on both architectures: builds both packages, runs
  `lintian` for information, installs the packages with `apt`, creates a
  device key line and a project, removes the packages while verifying that
  `server.ini` and the repository survive, then purges and verifies that the
  repository still survives.

Every job passes `BUILD_ROOT=$RUNNER_TEMP`; the Makefile keeps its default
build root for the development Mac and only accepts build directories beneath
the selected root. The arm64 jobs use the `ubuntu-24.04-arm` runner label and
the Intel macOS job uses `macos-15-intel`; both are hosted labels that GitHub
may rename in future, in which case only the matrix entries change.

## Debian and Raspberry Pi

```text
sudo apt install ./ck-git-hosting_0.1.0_arm64.deb
sudo ckgit-admin authorized-key --client-id mac-studio --public-key mac-studio.pub \
  | sudo tee -a /etc/ck-git-hosting/authorized_keys
```

The package installs the same tree as `packaging/install.sh`, with two
package-specific choices: the unit lives in `/usr/lib/systemd/system`, and
`authorized_keys` is created by the maintainer script rather than shipped, so
neither an upgrade nor a purge touches device keys by accident.
`server.ini` and the sshd drop-in are conffiles. `apt remove` keeps
configuration, state, repositories, and the account; `apt purge` removes
configuration and state and still keeps every bare repository and the
account, printing what was left behind.

The maintainer scripts skip `systemctl` when systemd is not running and skip
the sshd reload when `sshd` is absent, which is how the container test runs.
If `sshd -t` rejects the drop-in, it is renamed to `.disabled` and sshd is not
reloaded.

Build packages locally on a Debian host:

```text
make BUILD_ROOT=/tmp/ck BUILD_DIR=/tmp/ck/build all check
sh packaging/build-deb.sh --build-dir /tmp/ck/build --output /tmp/ck/dist
```

`--stage-only` writes the two package roots without `dpkg-deb`, which the
test suite uses on a workstation. `CKGIT_MAINTAINER` overrides the
`Maintainer` field.

Without `--version`, a local build is stamped `0.1.0+YYYYMMDD.HHMM.<commit>`,
which sorts above the plain release version and above every earlier local
build, so `apt install ./ck-git-hosting_*.deb` always upgrades. A rebuilt
package with an unchanged version is "already installed" to apt and needs
`dpkg -i` or `apt reinstall`. The release workflow passes the tag version
explicitly, so published packages carry the clean `0.1.0`.

## Other Linux distributions

Unpack the Linux tarball and run the installer from inside it; the unpacked
directory is a valid `--build-dir`:

```text
tar -xzf ck-git-hosting-0.1.0-linux-arm64.tar.gz
cd ck-git-hosting-0.1.0-linux-arm64
sudo sh packaging/install.sh --build-dir . --dry-run
sudo sh packaging/install.sh --build-dir . --yes
```

The archive is built against Debian 13's glibc. Older distributions build
from source with only Make and a C++20 compiler:

```text
make BUILD_ROOT=/tmp/ck BUILD_DIR=/tmp/ck/build all check
```

## macOS client with Homebrew

This repository is also a Homebrew tap: `Formula/ckgit.rb` builds the client
from the released source archive with a verified checksum.

```text
brew tap OWNER/ck-git-hosting https://github.com/OWNER/ck-git-hosting
brew install ckgit
brew install --HEAD ckgit
```

Replace `OWNER` with the GitHub account that hosts the repository. After
every release the workflow's final job rewrites the formula's `url`,
`sha256`, `homepage`, and `head` for the real repository and commits the
change to the default branch, so `brew upgrade ckgit` follows releases. Until
the first release only `--HEAD` installs succeed, because the placeholder
checksum cannot match.

Without Homebrew, unpack `ckgit-V-macos-arm64.tar.gz` or the `x86_64`
archive and copy `bin/ckgit` to a directory on `PATH`.

## Cutting a release

1. Update `VERSION` and land the change on the default branch with CI green.
2. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`.
3. The release workflow builds and tests every artifact, publishes the
   release with `SHA256SUMS`, and updates the Homebrew formula.

Release notes are generated from a fixed template; edit the release on GitHub
afterwards for anything version-specific.
