#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Every relative Markdown link under docs/operations, docs/protocol, and
# README.md must resolve to a file this repository actually tracks. This is
# what catches a link into the gitignored docs/planning/ (never shipped, so
# never followable by a reader of a release tarball or GitHub) and an
# ordinary typo or stale path left behind by a rename -- a doc reads as
# trustworthy only if every link in it actually goes somewhere.

set -eu

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo_root"

tracked=$(mktemp "${TMPDIR:-/tmp}/ckdocs-tracked.XXXXXX")
links=$(mktemp "${TMPDIR:-/tmp}/ckdocs-links.XXXXXX")
cleanup() { rm -f "$tracked" "$links"; }
trap cleanup EXIT HUP INT TERM
git ls-files >"$tracked"

fail=0

check_one() {
  file=$1
  target=$2
  case "$target" in
    http://*|https://*|mailto:*|'#'*) return 0 ;;
    /*)
      echo "$file: link target '$target' is an absolute filesystem path" >&2
      fail=1
      return 0
      ;;
  esac
  path=${target%%#*}
  [ -n "$path" ] || return 0  # a pure #anchor into the same page
  dir=$(dirname "$file")
  target_dir=$(dirname "$path")
  target_base=$(basename "$path")
  resolved_dir=$(cd "$repo_root/$dir/$target_dir" 2>/dev/null && pwd) || {
    echo "$file: link '$target' has no such directory" >&2
    fail=1
    return 0
  }
  case "$resolved_dir" in
    "$repo_root") resolved=$target_base ;;
    "$repo_root"/*) resolved=${resolved_dir#"$repo_root"/}/$target_base ;;
    *)
      echo "$file: link '$target' resolves outside the repository" >&2
      fail=1
      return 0
      ;;
  esac
  grep -qxF "$resolved" "$tracked" || {
    echo "$file: link '$target' -> $resolved is not a tracked file" >&2
    fail=1
  }
}

check_file() {
  file=$1
  grep -oE '\]\([^)]+\)' "$file" >"$links" || true
  while IFS= read -r raw; do
    [ -n "$raw" ] || continue
    target=${raw#](}
    target=${target%)}
    check_one "$file" "$target"
  done <"$links"
}

for file in README.md docs/operations/*.md docs/protocol/*.md; do
  [ -f "$file" ] || continue
  check_file "$file"
done

if [ "$fail" -ne 0 ]; then
  echo "docs link check failed" >&2
  exit 1
fi
echo "docs link check passed"
