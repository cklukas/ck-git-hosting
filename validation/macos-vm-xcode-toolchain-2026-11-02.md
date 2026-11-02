# macOS VM Xcode toolchain validation

After the user reviewed and accepted the Xcode and SDK agreements, the prepared
base was booted in setup mode. `sudo xcodebuild -license accept` was run inside
the guest; `xcodebuild -runFirstLaunch` then reported installation succeeded,
and `xcodebuild -checkFirstLaunchStatus` returned zero. The guest was shut down
before CI. Temporary guest setup directories were empty and removed.

An offline disposable clone ran a labelled gateway fixture on 2026-11-02. The
job checked first-launch status, compiled and ran a small C program with Xcode
27.2 (27B52), Apple Clang 21.0.0, and returned its executable in an artifact.
Run `00001793596453390367-de702f4d` recorded success, start epoch 1793596468,
end epoch 1793596512 (44 seconds including VM startup and shutdown). There is
no comparable compiler-job baseline. The source was the fixture commit
`342a41386af7fd319ea94e2cab85713c96e7d8d6`; the generated test script SHA-256
was `cf0a7636d08de859a4d208d2da3109b8faab9209520bd39738d9dafb1e858979`.
The returned artifact archive SHA-256 was
`3dbfbf4cc681547ada4896eab0680767c890d5342d4694d2dd867a8d2ee7f16f`.
The disposable VM and its work root were removed after confirmed shutdown;
the 324 KiB fixture and artifact are retained under
`/Volumes/S8T/tmp/ckgit-vm-xcode-e2e.E47W7DCz/fixture`. The prepared base
remains on PRO-BLADE at approximately 35 GiB.

This is a minimal compiler and artifact test, not an `xcodebuild` project or
application build, a production runner registration, an approved dependency
profile, or a real helper/host crash-recovery test.
