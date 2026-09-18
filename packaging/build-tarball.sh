#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds a distro-neutral release archive from a finished build directory.
# A server archive contains the binaries, the hook, the installer scripts,
# and the unit so `sh packaging/install.sh --build-dir .` works from the
# unpacked directory on any systemd host.  A client archive contains only
# the ckgit and ckdocs executables and documentation -- ckdocs travels with
# ckgit in both archives so a developer machine can preview documentation
# sites without a server installation.

set -eu

usage() {
  cat <<'USAGE'
Usage: build-tarball.sh --build-dir DIR --output DIR --platform NAME [--version V] [--client-only]

  --platform NAME  archive suffix such as linux-arm64, linux-amd64, macos-arm64
  --client-only    package only bin/ckgit (for macOS and other client hosts)
USAGE
}

build_dir=''
output=''
platform=''
version=''
client_only=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) [ $# -ge 2 ] || { usage >&2; exit 2; }; build_dir=$2; shift 2 ;;
    --output) [ $# -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
    --platform) [ $# -ge 2 ] || { usage >&2; exit 2; }; platform=$2; shift 2 ;;
    --version) [ $# -ge 2 ] || { usage >&2; exit 2; }; version=$2; shift 2 ;;
    --client-only) client_only=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

fail() {
  printf 'build-tarball.sh: %s\n' "$1" >&2
  exit 1
}

[ -n "$build_dir" ] && [ -n "$output" ] && [ -n "$platform" ] || { usage >&2; exit 2; }
case "$output" in
  /*) ;;
  *) fail "--output must be an absolute path" ;;
esac
case "$platform" in
  *[!a-z0-9-]*|'') fail "--platform may contain only lowercase letters, digits, and '-'" ;;
esac
script_dir=$(cd "$(dirname "$0")" && pwd)
source_root=$(cd "$script_dir/.." && pwd)
if [ -z "$version" ]; then
  [ -f "$source_root/VERSION" ] || fail "missing VERSION file"
  version=$(tr -d '[:space:]' <"$source_root/VERSION")
fi

if [ "$client_only" -eq 1 ]; then
  name="ckgit-$version-$platform"
else
  name="ck-git-hosting-$version-$platform"
fi
stage="$output/$name"
rm -rf "$stage"
install -d -m 0755 "$stage/bin" "$stage/docs/operations"
[ -f "$build_dir/bin/ckgit" ] || fail "missing $build_dir/bin/ckgit"
install -m 0755 "$build_dir/bin/ckgit" "$stage/bin/ckgit"
[ -f "$build_dir/bin/ckdocs" ] || fail "missing $build_dir/bin/ckdocs"
install -m 0755 "$build_dir/bin/ckdocs" "$stage/bin/ckdocs"
if [ "$client_only" -eq 0 ]; then
  for binary in ck-git-hostingd ck-git-shell ckgit-admin ck-ci-runnerd ck-pagesd; do
    [ -f "$build_dir/bin/$binary" ] || fail "missing $build_dir/bin/$binary"
    install -m 0755 "$build_dir/bin/$binary" "$stage/bin/$binary"
  done
  [ -f "$build_dir/hooks/post-receive" ] || fail "missing $build_dir/hooks/post-receive"
  install -d -m 0755 "$stage/hooks" "$stage/packaging/systemd" "$stage/packaging/deploy.d"
  install -m 0755 "$build_dir/hooks/post-receive" "$stage/hooks/post-receive"
  install -m 0755 "$script_dir/install.sh" "$stage/packaging/install.sh"
  install -m 0755 "$script_dir/uninstall.sh" "$stage/packaging/uninstall.sh"
  install -m 0755 "$script_dir/ck-git-hosting-deploy" "$stage/packaging/ck-git-hosting-deploy"
  install -m 0644 "$script_dir/systemd/ck-git-hosting.service" "$stage/packaging/systemd/ck-git-hosting.service"
  install -m 0644 "$script_dir/systemd/ck-ci-runner.service" "$stage/packaging/systemd/ck-ci-runner.service"
  install -m 0644 "$script_dir/systemd/ck-pages.service" "$stage/packaging/systemd/ck-pages.service"
  install -m 0644 "$script_dir/systemd/ck-git-hosting-deploy.service" "$stage/packaging/systemd/ck-git-hosting-deploy.service"
  install -m 0644 "$script_dir/systemd/ck-git-hosting-deploy.timer" "$stage/packaging/systemd/ck-git-hosting-deploy.timer"
  install -m 0644 "$script_dir/deploy.d/ck-git-hosting.conf.example" "$stage/packaging/deploy.d/ck-git-hosting.conf.example"
fi
install -m 0644 "$source_root/README.md" "$stage/README.md"
install -m 0644 "$source_root/VERSION" "$stage/VERSION"
for document in "$source_root"/docs/operations/*.md; do
  install -m 0644 "$document" "$stage/docs/operations/$(basename "$document")"
done
mkdir -p "$output"
(cd "$output" && tar -czf "$name.tar.gz" "$name")
rm -rf "$stage"
printf 'Built %s\n' "$output/$name.tar.gz"
