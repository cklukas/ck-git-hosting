# Provisioned inventory v1 (experimental)

This is a local verifier format, not yet a workflow declaration, approval
protocol, provisioning system, or CI admission gate. It currently supports
only protected macOS trees and fails closed on other platforms. It intentionally
rejects all symbolic links, hard-linked files, extended ACLs and extended
attributes; a provisioner must create a compatible protected tree rather than
silently following or dropping them. The existing Homebrew trees have not been
shown to satisfy this contract.

The manifest is UTF-8 text whose exact bytes end in a newline. The first five
lines are `ckgit-inventory-v1`, `lock=<sha256>`, `platform=macos`,
`architecture=<arm64|x64>`, and `abi=<lowercase-ci-label>`, in that order.
Each remaining line is a tab-separated `F`, lowercase content SHA-256,
four-digit octal mode, and safe relative path, or `D`, `-`, mode and path.
List every regular file and every directory below the root, including empty
directories, in strictly increasing bytewise path order. The identity is the
SHA-256 of the complete manifest bytes; both that identity and the exact lock
digest must be supplied independently by the caller. No links, special files,
duplicate paths, writable-by-group/other modes or unlisted entries are allowed.

The verifier requires root ownership and protected mode on every ancestor and
tree entry, checks macOS extended ACLs and attributes, refuses a volume with
ownership enforcement disabled, and refuses crossing into another filesystem.
It reads each file through a no-follow descriptor and checks metadata before
and after hashing. These checks assume the host administrator and kernel are
trusted; they do not provide protection against a malicious root process or
guarantee future contents if an administrator changes the tree after checking.
The eventual runner must keep the verified inputs protected for the entire job
and bind this identity to an approved, revocation-aware start permit.

Only unprivileged refusal tests have run. A positive test of a root-owned,
protected tree, content changes, missing entries and real job use remains
required before this verifier can be used for admission.
