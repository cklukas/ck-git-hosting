# Live claim transaction validation

On 2026-11-02, the Release `ckgit-unit-tests` target was incrementally built
from 07:45:37 to 07:46:10 UTC (33 seconds reported by the shell) in
`/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. A widely included queue
header caused 54 actions; the changed store and distributed test were compiled
and linked. A prior 54-action build in the same directory took 32 seconds.
No application was launched.

Input SHA-256 values:

- `include/ckgit/ci_store.hpp`: `6c76646e64ad2690c2954b47c1725f737872ce0686f8f355ac46c3b9b7efc824`
- `src/common/ci_store.cpp`: `813bdc8a45fe3a5e54a025828439d29009bf43e6b56bd7805ace58a391173887`
- `tests/unit/ci_distributed_tests.cpp`: `39ee527cde4f11ed2b2127e55493344380c3e46c2ac4d4fe1322ee86c3ef04f0`

The `ci-distributed` test passed in 5.51 seconds. It checked an exact live
lease and rejection of a different runner, different token, expired lease and
released lease. This transaction alone does not issue or consume a dependency
permit, validate an inventory, or authorize a job.
