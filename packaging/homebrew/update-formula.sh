#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Rewrites Formula/ckgit.rb for a published release.  The release workflow
# runs it with the tag version, the SHA-256 of the uploaded source archive,
# and the GitHub repository in OWNER/NAME form, then commits the result.

set -eu

if [ $# -ne 3 ]; then
  echo "Usage: update-formula.sh VERSION SHA256 OWNER/REPOSITORY" >&2
  exit 2
fi
version=$1
sha256=$2
repository=$3
case "$version" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "update-formula.sh: version must look like MAJOR.MINOR.PATCH" >&2; exit 1 ;;
esac
case "$sha256" in
  *[!0-9a-f]*|'') echo "update-formula.sh: sha256 must be lowercase hex" >&2; exit 1 ;;
esac
[ ${#sha256} -eq 64 ] || { echo "update-formula.sh: sha256 must be 64 hex characters" >&2; exit 1; }
case "$repository" in
  *[!A-Za-z0-9._/-]*|*/*/*|'') echo "update-formula.sh: repository must be OWNER/NAME" >&2; exit 1 ;;
  */*) ;;
  *) echo "update-formula.sh: repository must be OWNER/NAME" >&2; exit 1 ;;
esac

formula=$(cd "$(dirname "$0")/../.." && pwd)/Formula/ckgit.rb
[ -f "$formula" ] || { echo "update-formula.sh: missing $formula" >&2; exit 1; }
tmp="$formula.new"
sed \
  -e "s|^  homepage \".*\"$|  homepage \"https://github.com/$repository\"|" \
  -e "s|^  url \".*\"$|  url \"https://github.com/$repository/releases/download/v$version/ck-git-hosting-$version-source.tar.gz\"|" \
  -e "s|^  sha256 \".*\"$|  sha256 \"$sha256\"|" \
  -e "s|^  head \".*\", branch: \"master\"$|  head \"https://github.com/$repository.git\", branch: \"master\"|" \
  "$formula" >"$tmp"
grep -q "^  sha256 \"$sha256\"$" "$tmp" || { rm -f "$tmp"; echo "update-formula.sh: rewrite failed" >&2; exit 1; }
mv "$tmp" "$formula"
printf 'Updated %s for v%s\n' "$formula" "$version"
