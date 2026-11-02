# Dependency start-permit wire validation

On 2026-11-02, the Release `ckgit-unit-tests` target was incrementally built
in `/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. The final build ran
07:34:56–07:35:03 UTC (6 seconds reported by the shell), with the changed
protocol source and distributed tests compiled and linked. The preceding
07:34:26–07:34:38 build compiled 24 actions after the protocol header changed.
No comparable eight-action incremental baseline was recorded. No app ran.

Final SHA-256 inputs:

- `include/ckgit/ci_runner_protocol.hpp`: `f078f9c9007dabe3caf53acc60b31cec5ab90361d6aadf4e0335158972873269`
- `src/common/ci_runner_protocol.cpp`: `bea76922ea86a9e644ed52941fce7cb90577c55264eaf793e4a263ed0998c6b1`
- `tests/unit/ci_distributed_tests.cpp`: `c10f4fe7a25b17717e76e17cc55835c7c0c9b540d44cfa6900d0211e0f784d48`

The `ci-distributed` CTest passed in 3.78 seconds. It covers offer omission,
disabled/mismatched/duplicate offers and legacy protocol refusal, plus permit
identity fields, canonical numbers, malformed nonces and expiry bounds. It
does not test live negotiation, issuance, authentication, one-time consumption,
inventory validation or a real job start. Those functions do not yet exist.
