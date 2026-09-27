# Build from source

## Compile and test

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
[Deploying ck-git-hosting's own release](../operations/releases.md#deploying-ck-git-hostings-own-release)
and the [self-hosted release and deploy guide](../operations/05-releases-and-deploy.md)
for the end-to-end flow from a pushed tag to an upgraded server.
