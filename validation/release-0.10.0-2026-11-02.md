# Release validation: 0.10.0

Date: 2026-11-02

## Build

- Target/configuration: CMake + Ninja, macOS arm64, `Release`, `BUILD_TESTING=ON`
- Configure output: `/Volumes/S8T/tmp/ckgit-release-20261102/build`
- Install output: `/Volumes/S8T/tmp/ckgit-release-20261102/install-final`
- Clean release build: all 395 Ninja steps completed; the final incremental verification at 2026-11-02T11:48:17Z reported no work to do.
- Installed targets include `ckgit-admin`, `ck-ci-runnerd`, `ckgit-macos-vm`, and `ckgit-macos-vm-guest`.
- `ckgit`, `ckdocs`, `ckgit-admin`, and `ck-ci-runnerd` each report `0.10.0`.

## Documentation and package checks

- `ckdocs check --root /Volumes/PRO-BLADE/git/ck-git-hosting`: passed with 72 pages in 2 sites and no findings.
- Strict production portal build: passed with 72 pages, 62 assets, and 30 redirects.
- macOS arm64 client archive: `ckgit-0.10.0-macos-arm64.tar.gz`, including `ckgit`, `ckdocs`, release documentation, and the new macOS runner guides.

## Tests

- CTest excluding the aggregate `unit` wrapper: 19/19 passed, including native macOS policy, VM lifecycle, distributed CI, dependency review, process, archive, and Mermaid tests.
- The focused `ci-distributed` CTest passed.
- The aggregate `unit` wrapper remains nondeterministic in the accumulated pre-release suite: it has reported an existing dependency-review assertion and a VM-storage assertion on separate runs, while the isolated `ci-distributed` test passes. This does not affect the release build or the focused portal checks.

## Sandbox qualification

- Native macOS CI sandbox is Seatbelt-based and is not a VM. It is experimental, not enabled for hosted jobs by default, and its privileged lifecycle/crash-recovery qualification remains open.
- macOS Virtualization support remains separate experimental infrastructure and is not presented as the native sandbox.
