# Native-only macOS commit validation

The native-only source was prepared in the detached worktree
`/Volumes/S8T/tmp/ckgit-native-commit.UV6w7ZDN` at base `1f46455`. A clean
Release build of `ckgit-admin` and `ckgit-unit-tests` ran 11:37:15–11:38:00
UTC (44 seconds reported by the shell), with 231 compile/link actions. The
previous 85-second clean build used a different combined source snapshot and
targets, so it is not a comparable timing baseline. The build log confirmed
the native supervisor, runner, admin and unit-test executables linked.

During the first configure, the component installation rule changed before
configure completed, and its generated install script omitted the new rule.
The first install check caught this. CMake was regenerated explicitly; the
`macos-native-runner` component then installed all three executables, and each
installed file matched the build output byte for byte.

The unprivileged `macos-native-policy` test passed in 0.34 seconds. The first
unit test used a scratch directory inside the Git worktree and failed because
Git treated the fixture as part of the checkout. A second attempt correctly
rejected a scratch path outside the configured test root. Running the exact
built unit executable with `CKGIT_TEST_ROOT` and `TMPDIR` on a separate S8T
directory, and `CKGIT_SOURCE_ROOT` pointing to this worktree, passed all unit
tests. These are unprivileged tests; no runner or application was launched.

The root-owned supervisor lifecycle, detached process cleanup under the real
job identity, crash/restart recovery, and hosted artifact delivery remain
unqualified. The runner must not be registered or started for hosted jobs
until the privileged product setup qualification succeeds.

Key source SHA-256 values:

- `CMakeLists.txt`: `48e83d1ae6c169d13fe1e37ff83fc470656fc3bba47e46190d9767c05170cb0d`
- `src/ci-native-macos/main.cpp`: `a3705620c4628691ce2778e8b001454458df16f7746d3808ea91ef5b8bf0a986`
- `src/ci-native-macos/setup.inc`: `ace17b11aeeaa87f4aeef4836f2d7249c9240404f8d9ed3d6f8be8ce9fdf7a29`
- `src/ci-native-macos/qualify.inc`: `c753d4c5a2cb7e88f3cca9460f2a781126bdf28f74481f0a82b955928ec106c5`
- `src/common/macos_native.cpp`: `c42b7679ad947f170b10fb4c2e94bd35ccb67dd107b2c2f0f0250d3c6a372405`
- `src/common/ci_runner.cpp`: `ee01e1b928c70a56e855cbf03244efa123b5529dad705ee7f22b762b3a5df66c`
- `src/ci-runner/main.cpp`: `6919734130c4f66693f6818654e8076601f763d400c0c8ec0e7710a70be947db`
- `src/admin/main.cpp`: `4af63b938f8f94470bf5676be95e4f1665c8be315f11937713be66fb1b89610c`
