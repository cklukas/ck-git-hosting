# Provisioned inventory verifier validation

On 2026-11-02 the Release `ckgit-unit-tests` target was built in
`/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. The final incremental build
ran 07:17:53–07:17:54 UTC (1 second reported by the shell), compiled the
changed verifier and linked the test executable. A prior build at
07:17:27–07:17:29 compiled the verifier and inventory tests. Logs showed the
changed translation units and links; no comparable six-action incremental
baseline was established. No app or VM was launched.

Final SHA-256 inputs:

- `include/ckgit/ci_provisioned_inventory.hpp`: `925f40986d7009bdee781f40ca7d33bb16603b53440af9e04f96c6318e520aae`
- `src/common/ci_provisioned_inventory.cpp`: `688ee2ab15b0a4414baa057f3dfe8780e1c1b9d2cda80337d04d9d83addd45c4`
- `tests/unit/ci_provisioned_inventory_tests.cpp`: `aac11f1fe7a206930cd6428ac06e789a60f17f70457b9a492901995d642a4ad6`
- `tests/unit/test_main.cpp`: `b309b982be7f20629b6ad7b1dbebe598607a94b89360e595eadedd74585f6475`

The `ci-dependency` CTest passed in 2.81 seconds. The new unprivileged tests
exercise rejection of incorrect manifest identity or target, unsafe paths,
links and a user-owned tree. No root-owned positive fixture, content-tampering
test on a protected tree, start-permit integration, or real runner admission
has been qualified. The verifier is intentionally not connected to production.
