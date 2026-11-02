#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -eu
[ "$#" -eq 3 ] && [ -n "${TMPDIR:-}" ] || exit 2
task=$(mktemp -d "${TMPDIR%/}/vm-share.XXXXXX")
cleanup() {
  for f in "$task/share/ckgit-macos-vm-guest" "$task/share/install-vm-guest.sh" \
           "$task/prefix/bin/ckgit-admin" "$task/prefix/bin/ckgit-macos-vm-guest" \
           "$task/prefix/share/ck-git-hosting/macos/install-vm-guest.sh" "$task/log"; do
    if [ -e "$f" ] || [ -L "$f" ]; then /bin/rm "$f"; fi
  done
  rmdir "$task/share" "$task/prefix/bin" "$task/prefix/share/ck-git-hosting/macos" \
        "$task/prefix/share/ck-git-hosting" "$task/prefix/share" "$task/prefix" "$task" 2>/dev/null || true
}
trap cleanup EXIT
umask 077
mkdir -p "$task/prefix/bin" "$task/prefix/share/ck-git-hosting/macos"
cp "$1" "$task/prefix/bin/ckgit-admin"
cp "$2" "$task/prefix/bin/ckgit-macos-vm-guest"
cp "$3" "$task/prefix/share/ck-git-hosting/macos/install-vm-guest.sh"
admin="$task/prefix/bin/ckgit-admin"
"$admin" runner macos-vm prepare-share "$task/share" > "$task/log" 2>&1
cmp "$2" "$task/share/ckgit-macos-vm-guest"
cmp "$3" "$task/share/install-vm-guest.sh"
[ "$(stat -f %Lp "$task/share")" = 700 ] || exit 1
if "$admin" runner macos-vm prepare-share "$task/share" > "$task/log" 2>&1; then
  printf '%s\n' 'existing setup share was overwritten' >&2; exit 1
fi
cmp "$2" "$task/share/ckgit-macos-vm-guest"
/bin/rm "$task/prefix/bin/ckgit-macos-vm-guest"
ln -s "$2" "$task/prefix/bin/ckgit-macos-vm-guest"
if "$admin" runner macos-vm prepare-share "$task/bad-share" > "$task/log" 2>&1; then
  printf '%s\n' 'symlinked installed guest was accepted' >&2; exit 1
fi
[ ! -e "$task/bad-share" ] || { printf '%s\n' 'failed share left a directory' >&2; exit 1; }
printf '%s\n' 'VM guest setup-share preparation passed'
