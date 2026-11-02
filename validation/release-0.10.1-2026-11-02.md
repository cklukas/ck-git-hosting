# Release validation: 0.10.1

Date: 2026-11-02

## Scope

This patch release contains all changes on `master` after 0.10.0. In
particular, it includes the ckdocs portal selector links and icons, the
experimental native macOS Seatbelt sandbox, the dependency-review and VM
infrastructure, and the CI portability fixes listed in `CHANGES.md`.

## Build and test record

- Target/configuration: Make `all check`, macOS arm64, default Release flags.
- Build start/end: `2026-11-02T13:23:40Z` to `2026-11-02T13:35:26Z`;
  elapsed `706` seconds. This is within the recent 11–19 minute clean-CI
  baseline for this repository.
- Source fingerprint: `f50e869baecc3f3c2d51ef60fe1b64fd5e53750cd16a195f1c7eae2824279b77`
  (`git archive --format=tar HEAD | shasum -a 256`).
- Verified build output: `/Volumes/S8T/tmp/ckgit-release-0.10.1-20261102/build`.
  `make all check` passed, including the complete unit suite, integration
  suite, documentation site checks, and archive tests.
- The built tools report `0.10.1+g80176a2fa518`. The verified binary hashes
  are recorded below for the retained build:

  ```text
  ckgit          8b354f7ea380744e28f3097b1bf72da2b8468b26aee840305d478299b0c37438
  ckdocs         5830183cfb5d6d4c3ad02ff986a8ca1320c70fa0d9544a9e6bf0b84799736ddb
  ck-ci-runnerd  03a7b7702157a7d6d3cba4665da5732ba2c7b22e5b985c0df6ace2554c438898
  ck-pagesd      e02770e7249d858941ae316fb0ade08871433dc4118904d7e98b536da48c5fb3
  ck-git-hostingd d0ad2dd5c9eeb8ae64a11d012d09f5949bcbc401383d3d3ea1a147561adef70b
  ```

- `ckdocs check --root /Volumes/PRO-BLADE/git/ck-git-hosting` passed:
  72 pages in 2 sites, no findings.
- GitHub validation runs: CI `41452462913`; Pages `41452462712`.

## Sandbox qualification

The native macOS CI sandbox is Seatbelt-based and is not a VM. It remains
experimental, is not enabled for hosted jobs by default, and still requires
privileged lifecycle and crash-recovery qualification. The separate macOS
Virtualization infrastructure is not presented as the native sandbox.
