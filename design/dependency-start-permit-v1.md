# Dependency start permit v1 (wire draft, disabled)

There is no live start-permit operation. Gateways continue to advertise
`dependency_admission=0`, and required-lock jobs remain queued. A future
protocol 3 server may explicitly advertise `dependency_admission=permit-v1`
only after the whole admission path is implemented and qualified. Missing,
duplicate, disabled, mismatched or legacy-protocol offers are not support.

The canonical newline-terminated response has these ordered lines:

```text
dependency-permit-v1
project=<project>
commit=<exact-git-object-id>
lock_sha256=<reviewed-lock-sha256>
inventory_sha256=<separate-protected-output-inventory-sha256>
platform=<macos|windows|linux>
architecture=<arm64|x64>
abi=<ci-label>
job_id=<job-id>
lease_sha256=<sha256-of-exact-lease-token>
nonce=<32-lowercase-hex-digits>
attempt=<canonical-positive-decimal>
issued=<epoch-seconds>
expires=<epoch-seconds>
```

The maximum lifetime is 120 seconds; expiry is exclusive. The two digests have
different meanings and are independently verified. The lease digest identifies
the exact lease without exposing its bearer token in the permit. The future
server must generate an unpredictable nonce, issue the permit while holding
the exact review lock and validating the active lease, and durably enforce
single consumption. Issuance is the admission linearization point: a revocation
that commits before issuance blocks it; policy must explicitly handle jobs
already admitted when a later revocation occurs. Inputs must be provisioned and
verified before issuance, and remain protected for the job. Never hold an
approval lock while provisioning or building.

The current parser only validates syntax, identity shapes and the time window.
It does not authenticate a message, compare it with a live lease, generate or
store a nonce, enforce one use, verify an inventory, negotiate a working
capability, or start a process. No caller may use it as authorization.
