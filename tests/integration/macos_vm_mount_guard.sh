#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -eu
[ "$#" -eq 1 ] && [ -n "${TMPDIR:-}" ] || exit 2
helper=$1
task=$(mktemp -d "${TMPDIR%/}/vm-mount-guard.XXXXXX")
cleanup() {
  if [ -L "$task/other-mount" ]; then /bin/rm "$task/other-mount"; fi
  if [ -f "$task/log" ]; then /bin/rm "$task/log"; fi
  rmdir "$task" 2>/dev/null || true
}
trap cleanup EXIT
ln -s / "$task/other-mount"
if "$helper" validate "$task/other-mount" "$task" > "$task/log" 2>&1; then
  printf '%s\n' 'VM helper accepted a path resolving to a different mount' >&2; exit 1
fi
case "$(cat "$task/log")" in
  *'not mounted at its expected path'*) ;;
  *) cat "$task/log" >&2; exit 1;;
esac
missing="/Volumes/ckgit-unmounted-${task##*.}"
if [ -e "$missing" ] || [ -L "$missing" ]; then
  printf '%s\n' 'refusing to test a preexisting volume name' >&2; exit 1
fi
if "$helper" validate "$missing/base" "$task" > "$task/log" 2>&1; then
  printf '%s\n' 'VM helper accepted a missing volume' >&2; exit 1
fi
case "$(cat "$task/log")" in
  *'not mounted at its expected path'*) ;;
  *) cat "$task/log" >&2; exit 1;;
esac
printf '%s\n' 'VM helper rejected missing and mismatched mounts'
