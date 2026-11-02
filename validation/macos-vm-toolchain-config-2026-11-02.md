# macOS VM toolchain configuration validation

On 2026-11-02, a Release incremental build of `ck-ci-runnerd` and
`ckgit-unit-tests` ran from 03:57:19 to 03:57:27 UTC (7 seconds reported by
the shell) in `/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. The build
log showed both changed production translation units and the changed test
translation unit compiled and their targets linked. The existing build had
previously been used for this configuration; no comparable three-source
incremental duration baseline was established. No application was launched.

SHA-256 inputs:

- `src/common/runner_config.cpp`: `d2ce6785b647a13cfae5d3cf55e3c11f3ab0e1fdde538eb5d41ffae138447cfc`
- `src/ci-runner/main.cpp`: `351a291dedee1cf0e44ab3b60722f3941330d23874e11e79abd5d4f3e06bacc2`
- `tests/unit/ci_distributed_tests.cpp`: `9175882eaa584ffb773f33bb659967c2e1177c4fc421d02e7fd513c5c50560c8`

The `ci-distributed` CTest passed in 4.26 seconds. Its new cases verify that
VM configuration refuses a Windows toolchain root, custom toolchain path,
and toolchain environment variable. This does not qualify a real guest build.
