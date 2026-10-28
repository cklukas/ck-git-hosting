# Pinned libarchive source

This directory contains the unmodified libarchive **3.8.10** release archive.
It is permanent build input, not a downloaded build cache. Upstream source
extraction and compilation belong in the chosen out-of-tree build directory.

- Source: https://www.libarchive.org/downloads/libarchive-3.8.10.tar.xz
- Release: https://github.com/libarchive/libarchive/releases/tag/v3.8.10
- SHA-256: `56cad765459d5ee25a3b761f90c47d99379d28297fbfb72d908880d6b875fb96`
- Size: 6,735,604 bytes

The source downloaded from libarchive.org was checked against the release
asset digest reported by GitHub on 2026-10-26. Configuration verifies that
digest again before extraction. The build never downloads a dependency.
`COPYING` is copied verbatim from the release; the archive preserves all
individual source and build-file license notices.

The CMake target builds a static library with optional compression, crypto,
XML, ACL and extended-attribute dependencies disabled. ck-git-hosting's
archive formats are uncompressed TAR. The caller must register only the
required TAR reader/writer, not all libarchive formats or external filters.
MSVC uses the static runtime so no administrator-installed redistributable
is needed. This build target alone does not replace the application's existing
tar calls; integration and bounded archive handling are still required.

For an independent qualification, configure this directory with an explicit
scratch build path and build `archive_static`. Follow the project's `TMPDIR`
and Windows `TEMP`/`TMP` requirements. Do not configure inside this directory.

The standalone `ckgit-archive-smoke` target checks the linked library version
and a UTF-8 TAR roundtrip (pathname, size, payload and end-of-archive). Run it
through CTest after building that target. libarchive deliberately returns NFD
pathnames on macOS; the test checks those exact UTF-8 bytes there and NFC on
Windows. Application integration must account for that normalization when
comparing names. The test's process-wide UTF-8 locale setup is not a proposed
locale policy for multithreaded application code.

Updates must replace the archive, pinned digest, notices and version together,
then repeat native builds and archive interoperability/security tests. Never
substitute a system library or a PATH-resolved executable silently.
