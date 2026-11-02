# Dependency review transaction validation

On 2026-11-02, the Release `ckgit-unit-tests` target was incrementally built
from 06:13:34 to 06:13:47 UTC (12 seconds reported by the shell) in
`/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. The build compiled the
changed dependency store and test sources and linked the test executable; 25
actions were required because other shared inputs also had changed. No
comparable 25-action incremental baseline was established. No app was launched.

SHA-256 inputs:

- `include/ckgit/ci_dependency_store.hpp`: `5c957eab4a93fb79bde6bcd7d72c1a9d14ef4e0ccc6120243f6b4e58f37a782a`
- `src/common/ci_dependency_store.cpp`: `c9b3a9c1c4e700628bd76eb5e1f4c1f8a7960105aed71d74473bb3cdd58af46c`
- `tests/unit/ci_dependency_store_tests.cpp`: `08d8f3054921d02c34f4867940478a4c439598d34436d79fb36a8eef7974057b`

The `ci-dependency` CTest passed in 2.63 seconds. It exercises refusal for a
different commit or platform, exclusion of a concurrent revocation during a
transaction, and refusal after that revocation. This tests review locking;
it does not verify provisioning, inventory identity or job admission.

The exact-commit integration was built incrementally in the same Release
directory from 06:20:24 to 06:20:29 UTC (5 seconds, eight actions). The changed
commit reader and its test were compiled, and the admin and unit executables
were linked. No comparable eight-action incremental baseline was recorded.
The `ci-dependency` test passed in 3.09 seconds, including real Git commits,
exact lock digests, wrong targets, revocation, and an unrequested commit with
the same lock. The three changed input SHA-256 values were:

- `include/ckgit/ci_dependency_commit.hpp`: `1bac2994636f6e7c416d826760046868c076af03bc996f996cbfe2e92d9a877a`
- `src/common/ci_dependency_commit.cpp`: `1a5a22afeda4d9842089b9d4f79904bfee351353e261f15d285efaba8c58ce2f`
- `tests/unit/ci_dependency_commit_tests.cpp`: `9493dd123f138cc1ae4b163df65b127b3580926c0c4ef9dabc13d76156fc358d`
