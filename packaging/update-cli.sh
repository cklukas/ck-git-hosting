#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Updates the local ckgit client from a ck-git-hosting release built by the
# server's own CI (WP7). The server builds Linux binaries, which will not run
# on macOS or another architecture, so this fetches the release's SOURCE
# tarball over the ordinary ckgit transport and builds ckgit locally, pinned
# to the exact release deployed to the server.
#
#   sh packaging/update-cli.sh                       # newest release -> /usr/local/bin
#   sh packaging/update-cli.sh --tag v0.1.0 --prefix ~/bin
#
# Needs only an existing ckgit paired with the server (ckgit setup, or an
# existing client.ini) and, unlike earlier versions of this script, no SSH
# login or sudo access on the server itself: `ckgit release download` reaches
# the release through the same control RPC and dashboard tunnel every other
# ckgit command uses, reading the target host from client.ini.
set -eu

prefix=${CK_PREFIX:-/usr/local/bin}
build_root=${CK_BUILD_ROOT:-}
want_tag=''

while [ $# -gt 0 ]; do
  case "$1" in
    --tag) want_tag=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --build-root) build_root=$2; shift 2 ;;
    -h|--help) echo "usage: update-cli.sh [--tag TAG] [--prefix DIR] [--build-root DIR]"; exit 0 ;;
    *) echo "update-cli.sh: unknown option $1" >&2; exit 2 ;;
  esac
done

fail() { printf 'update-cli.sh: %s\n' "$1" >&2; exit 1; }
command -v make >/dev/null 2>&1 || fail "make is required to build the client"
command -v ckgit >/dev/null 2>&1 || fail "ckgit is required to fetch the release (install it once, then re-run this script to update it)"

work=$(mktemp -d "${TMPDIR:-/tmp}/ckcli.XXXXXX")
trap 'rm -rf "$work"' EXIT

# 1. Fetch and verify the release's packaged bundle. The workflow's own
#    artifacts: block names it "packages" (see .ckgit/ci.yml -- "release" is
#    reserved for the tag build's own release record).
set -- --asset packages --into "$work"
[ -z "$want_tag" ] || set -- "$@" --tag "$want_tag"
download_output=$(ckgit release download ck-git-hosting "$@" 2>&1) \
  || fail "could not download the ck-git-hosting release ($download_output)"
printf 'update-cli: %s\n' "$download_output"
tar -xf "$work/packages.tar" -C "$work"
src_tar=$(find "$work" -name 'ck-git-hosting-src.tar.gz' | head -1)
[ -n "$src_tar" ] || fail "the release has no source tarball (ck-git-hosting-src.tar.gz)"
mkdir -p "$work/src"
tar -xzf "$src_tar" -C "$work/src"

# 2. Build just the client, stamped with the release's exact baked version so
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

# 3. Install.
if install -m 0755 "$bin" "$prefix/ckgit" 2>/dev/null; then
  :
else
  printf 'update-cli: %s is not writable; installing with sudo\n' "$prefix"
  sudo install -m 0755 "$bin" "$prefix/ckgit"
fi
printf 'update-cli: installed ckgit to %s/ckgit\n' "$prefix"
"$prefix/ckgit" --version 2>/dev/null | head -1 || true
