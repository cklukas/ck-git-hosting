# Windows HCS execution-report validation

The narrowly scoped execution-report parser change from local commit
`fbab714` was applied to the shared workspace without cherry-picking or
overwriting its other uncommitted changes. A Release incremental build of
`ckgit-unit-tests` ran from 06:33:22 to 06:33:29 UTC (7 seconds reported by
the shell) in `/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`; its log
showed the changed parser and distributed test compiled and linked. No
comparable eight-action incremental duration baseline was established. No
application was launched.

Input SHA-256: `src/common/ci_types.cpp`
`a49aa13ed2d405821e5d990311acf36882a259201d77013fea37431afcc3272c`;
`tests/unit/ci_distributed_tests.cpp`
`32f6d3e532a2ffbadd68591a8297f9e65ab754cde975ce319ddc3bf1e58cedef`.
The `ci-distributed` CTest passed in 4.51 seconds, covering HCS report
round-trip and rejection of the same isolation value for Linux and macOS.
This does not qualify or enable a Windows VM runner.
