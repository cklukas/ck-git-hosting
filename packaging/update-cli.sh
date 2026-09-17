#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Updates the local ckgit client from a ck-git-hosting release built by the
# server's own CI. The server builds Linux binaries, which will not run on
# macOS or another architecture, so this fetches the release's SOURCE tarball
# and builds ckgit locally, pinned to the exact release deployed to the server.
#
#   sh packaging/update-cli.sh                       # newest release -> /usr/local/bin
#   sh packaging/update-cli.sh --tag deploy-7 --prefix ~/bin
#
# --ssh-host is the admin SSH login to the server (needs sudo there to read the
# root-owned release store); it is separate from the ckgit transport account.
set -eu

ssh_host=${CK_SSH_HOST:-rpi4}
prefix=${CK_PREFIX:-/usr/local/bin}
build_root=${CK_BUILD_ROOT:-}
want_tag=''
state_root=/var/lib/ck-git-hosting/state
releases="$state_root/releases/ck-git-hosting"

while [ $# -gt 0 ]; do
  case "$1" in
    --ssh-host) ssh_host=$2; shift 2 ;;
    --tag) want_tag=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --build-root) build_root=$2; shift 2 ;;
    -h|--help) echo "usage: update-cli.sh [--ssh-host HOST] [--tag TAG] [--prefix DIR] [--build-root DIR]"; exit 0 ;;
    *) echo "update-cli.sh: unknown option $1" >&2; exit 2 ;;
  esac
done

fail() { printf 'update-cli.sh: %s\n' "$1" >&2; exit 1; }
command -v make >/dev/null 2>&1 || fail "make is required to build the client"

# 1. Resolve the release tag on the server (newest with a committed record).
tag="$want_tag"
if [ -z "$tag" ]; then
  tag=$(ssh "$ssh_host" "sudo sh -c 'for d in \$(ls -1dt $releases/*/ 2>/dev/null); do [ -f \"\$d/release.ini\" ] && basename \"\$d\" && break; done'") \
    || fail "could not reach $ssh_host or read the release store"
fi
[ -n "$tag" ] || fail "no ck-git-hosting release found on $ssh_host"
printf 'update-cli: fetching release %s from %s\n' "$tag" "$ssh_host"

work=$(mktemp -d "${TMPDIR:-/tmp}/ckcli.XXXXXX")
trap 'rm -rf "$work"' EXIT

# 2. Fetch and unpack the release, then the source tarball inside it. The
#    workflow's artifacts: block is named "packages" (an artifact named
#    "release" would collide with the release record); older releases built
#    before that rename still carry release.tar, so try both.
ssh "$ssh_host" "sudo sh -c 'cat $releases/$tag/packages.tar 2>/dev/null || cat $releases/$tag/release.tar'" \
  >"$work/packages.tar" || fail "could not fetch the packages (or legacy release) asset for $tag"
tar -xf "$work/packages.tar" -C "$work"
src_tar=$(find "$work" -name 'ck-git-hosting-src.tar.gz' | head -1)
[ -n "$src_tar" ] || fail "release $tag has no source tarball (ck-git-hosting-src.tar.gz)"
mkdir -p "$work/src"
tar -xzf "$src_tar" -C "$work/src"

# 3. Build just the client, stamped with the release's exact baked version so
#    the updated client reports the same version as the deployed server (the
#    source tarball carries no .git for the Makefile to derive it from).
[ -n "$build_root" ] || build_root="$work/b"
mkdir -p "$build_root"
build_version=$(cat "$(find "$work" -name build-version | head -1)" 2>/dev/null || true)
( cd "$work/src" && make BUILD_ROOT="$build_root" BUILD_DIR="$build_root/build" \
    ${build_version:+CKGIT_BUILD_VERSION="$build_version"} client ) \
  || fail "building ckgit from the release source failed"
bin="$build_root/build/bin/ckgit"
[ -x "$bin" ] || fail "client build produced no ckgit binary"

# 4. Install.
if install -m 0755 "$bin" "$prefix/ckgit" 2>/dev/null; then
  :
else
  printf 'update-cli: %s is not writable; installing with sudo\n' "$prefix"
  sudo install -m 0755 "$bin" "$prefix/ckgit"
fi
printf 'update-cli: installed ckgit (release %s) to %s/ckgit\n' "$tag" "$prefix"
"$prefix/ckgit" --version 2>/dev/null | head -1 || true
