# Windows isolation-checked runner claims — validation

The working-tree changes based on `27396f0` add reserved `lpac` and
`hyperv-hcs` labels to the runner claim protocol and enforce them before a
lease is issued. The changed tracked-file diff SHA-256 at validation was
`5a0ae4a7e493ddc2f286f62aa5e8f585087fb3ab269f04d46be1e4165ef32e49`.

- macOS arm64 Release build, 22:40:59–22:43:24 UTC: the unit and process
  suites passed (2/2, 65.15 seconds); the strengthened process fixture passed
  again after an incremental rebuild. The unit suite covers matching LPAC and
  HCS claims, contradictory labels, legacy and mismatched claims, no lease on
  refusal, protocol negotiation and an actual gateway claim.
- Windows x64 MSVC Release build, 22:41:55–22:48:06 UTC: runner and process
  targets built; the native process and runner-broker fixtures passed. The
  verified `ck-ci-runnerd.exe` SHA-256 was
  `c38c7faf07f5826893064970510c6cea473ff7b28c420b5ce61fe2add76b3c6f`.
- After strengthening the fixture to require an LPAC value in the actual
  runner claim, the Windows process fixture and runner-broker fixture passed
  again. They cover old protocol and old protocol-2 servers, a server-reported
  label conflict, and the checked LPAC claim. The test fixture source hashes
  were `6eca8d3c2488cf7ae128e8d3d772980b762ff40e3d3996fde8c8d9eae022dc07`
  and `311a0b5e4471bf21b1d091e9d808baa6655eca913caa529fb281e0f9294328fc`.

These were isolated tests; no deployed server or runner was updated. An actual
HCS guest job was not rerun with the new claim protocol. This check depends on
an authenticated, trusted runner reporting its configured backend and is not
remote attestation against a compromised runner.
