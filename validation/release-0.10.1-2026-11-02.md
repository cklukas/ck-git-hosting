# Release validation: 0.10.1

Date: 2026-11-02

## Scope

This patch release contains all changes on `master` after 0.10.0. In
particular, it includes the ckdocs portal selector links and icons, the
experimental native macOS Seatbelt sandbox, the dependency-review and VM
infrastructure, and the CI portability fixes listed in `CHANGES.md`.

## Build and test record

The final clean release build, installed artifact paths, source and asset
fingerprints, and CI run identifiers are recorded here after validation.

## Sandbox qualification

The native macOS CI sandbox is Seatbelt-based and is not a VM. It remains
experimental, is not enabled for hosted jobs by default, and still requires
privileged lifecycle and crash-recovery qualification. The separate macOS
Virtualization infrastructure is not presented as the native sandbox.
