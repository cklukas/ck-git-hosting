# Dependency queue hold validation

On 2026-11-02, the Release `ckgit-unit-tests` target was built from 07:27:36
to 07:28:09 UTC (32 seconds reported by the shell) in
`/Volumes/S8T/tmp/ckgit-vm-integrated.Ko9BS8/build`. Changing a widely used
queue-record header caused 54 compile/link actions; the output included the
modified queue, gateway and distributed-test sources. No comparable 54-action
incremental baseline was established. No app was launched.

Input SHA-256 values:

- `include/ckgit/ci_store.hpp`: `70a8c59592caaaa271a0a0a3d87dee4a5b830c019d7854764cf7a22b634acbe0`
- `src/common/ci_store.cpp`: `1772ecce125e63d2d34974005f76dbc003546f66595afd1157e4ae5ae29a11e1`
- `src/common/ci_gateway.cpp`: `77924643eb3e5879c658202b77e70a44294ef242f87b9dfdde2ddb889a358e14`
- `tests/unit/ci_distributed_tests.cpp`: `a70a4c56ad62a49af6a33daeac9f96169a33f8a38ef04fcc652460170e6d67d8`

The `ci-distributed` CTest passed in 3.87 seconds. It verifies a required-lock
record is held for protocol versions 1, 2 and a simulated later version, can
be cancelled without a lease, and does not prevent an ordinary legacy job from
being claimed. It also checks the gateway's explicit `dependency_admission=0`
response. No workflow creates these records yet; this is not a capability
negotiation, inventory check, start permit, or end-to-end admission test.
