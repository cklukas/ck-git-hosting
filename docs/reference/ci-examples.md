# CI workflow examples

## Worked examples

**A plain `make` project:**

```yaml
version: 1
on: { branches: [main], tags: [v*] }
jobs:
  - name: build-and-test
    steps:
      - run: [make, all]
      - name: tests
        run: make check
    artifacts:
      name: build
      paths: [dist]
```

**CMake, ccache, and one pinned sister:**

```yaml
version: 1
sisters:
  - name: ckvision
    ref: v0.5.0
cache:
  - name: ccache
    env: [CCACHE_DIR]
jobs:
  - name: build
    env: { CCACHE_MAXSIZE: "8G" }
    steps:
      - script: |
          cmake -S . -B build -G Ninja \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
          cmake --build build
      - name: tests
        run: [ctest, --test-dir, build, --output-on-failure]
```

**A docs site published from the default branch, with releases on tags:**

```yaml
version: 1
on: { branches: [main], tags: [v*] }
jobs:
  - name: docs
    steps:
      - run: [ckdocs, build, --root, ., --out, public, --strict]
    artifacts:
      name: packages
      paths: [dist]
pages:
  path: public
```

Every push to `main` builds and, on success, publishes `public` as the
project's Pages site — `--strict` fails the build, before publishing ever
sees it, on a broken link or heading fragment (see [Documentation sites
from Markdown](../operations/07-docs-sites.md)). A tag push instead produces a durable
release from the same `packages` artifact, and does **not** touch Pages at
all — the two outcomes are mutually exclusive per run, driven entirely by
whether the triggering ref was a tag.

**This project's own workflow, annotated** — the real, in-repository
`.ckgit/ci.yml` this project builds, tests, and releases itself with:

```yaml
version: 1
on:
  branches: [master]
  tags: [v*]
jobs:
  - name: build-test-package
    steps:
      - name: build
        script: |
          # computes a version string from CKGIT_COMMIT or .ckgit/build-commit
          # (see §8), then: make BUILD_ROOT=... CKGIT_BUILD_VERSION=... all
      - name: check
        script: |
          # make ... check -- the unit binary plus every tests/integration/*.sh
      - name: docs
        script: |
          # ckdocs, just built by the build step, into ./public --strict
          # (see Documentation sites from Markdown)
      - name: package
        script: |
          # refuses a tag that does not match VERSION, builds .debs and a
          # source tarball with packaging/build-deb.sh, writes build-version
    artifacts:
      # never "release": that name is reserved for the tag build's own
      # release record
      name: packages
      paths: [dist]
pages:
  path: public
```

The full workflow is in the repository's `.ckgit/ci.yml`. Its four steps
share state through the runner's `$TMPDIR`. The `packages` artifact holds
the release files; `release` is a reserved artifact name. The `pages:`
block publishes documentation after a successful default-branch build.
