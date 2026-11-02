# macOS VM base permissions validation

On 2026-11-02, a Release incremental build of `ckgit-admin` and
`ckgit-unit-tests` ran from 03:58:30 to 03:58:37 UTC (6 seconds reported by
the shell) in `/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. The log
confirmed compilation of all four changed C++/Objective-C++ inputs and
relinking of the helper, runner, admin and unit-test executables. No
comparable four-source incremental duration baseline was established. No
application or VM was launched.

SHA-256 inputs:

- `src/common/macos_vm_storage.cpp`: `6c507969a821cecaf40a747f22ca99a378ee5a8f6ccf12aecd37ae5c988bc271`
- `src/common/macos_vm_workspace.cpp`: `d37441df33dcc531b9b3196caf221e0c697aacea3afad7095277966d648d9744`
- `src/ci-vm-macos/main.mm`: `a9603000859f4f22060e1cd19d839a304f5dd07ff009d1e8fde6dbbb866c9462`
- `tests/unit/ci_distributed_tests.cpp`: `8006e269f5fec84662dea70cdbe021d80d4544ecd3adf6759f068aa9d6d90e52`

The `ci-distributed` CTest passed in 3.33 seconds, including rejection of a
mode-0755 base directory. The prepared base and its disk were separately
observed to have modes 0700 and 0600 respectively. This test does not qualify
an Xcode build or real VM crash recovery.

A subsequent real-VM gateway test passed with the updated helper and runner:
an unlabelled job remained queued, a job labelled `macos-vm,arm64` ran in a
disposable VM and returned its artifact, and the test drained its remaining
job. The VM work root was empty apart from the runner lock after shutdown;
the lock and work root were removed. The 316 KiB-scale test fixture remains
under `/Volumes/S8T/tmp/ckgit-vm-private-e2e.ypF23RUq/fixture`; it contains no
VM image. The first attempt to invoke the non-executable test script directly
failed before the test started; the successful run invoked it with `/bin/sh`.

For that test, `ck-git-shell` was relinked at 03:58:59 UTC in the same Release
build directory; the incremental build completed in less than a second and
showed the link step. No comparable link-only duration baseline was recorded.
The verified helper signature passed `codesign --verify --strict`. SHA-256:
helper `a353a75f010675ce28eff327d71d3a23735df941e0950273bfee49726e77af76`,
runner `ad9097539d3fdd7514bb019c5ab001db532a9984360b31d33d18a07775ab51cc`,
shell `35164ebf6ee6c5cd8d7318b86eeae3e87624ed9df2d6c988ef65f4a3812a0c1d`,
test script `640d79d5e217ad34a14a83dc10b3d08ea138e1b39f43d320fccefe46eb5ad1c8`.
