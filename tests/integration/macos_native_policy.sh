#!/bin/sh
# Unprivileged check of the actual production policy, not UID supervision.
set -eu
bin=$1
parent=$2
export TMPDIR="$parent"
[ -d "$TMPDIR" ]
fixture=$(mktemp -d "$TMPDIR/native-policy.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
ulimit -c 0
unset CKGIT_MACOS_SANDBOX
mkdir -p "$fixture/work/src" "$fixture/work/tmp" "$fixture/work/home"
cp "$bin" "$fixture/work/src/probe"
cmp "$bin" "$fixture/work/src/probe"
printf 'outside file\n' > "$fixture/outside"
"$bin" policy "$fixture/work" > "$fixture/profile.sb"
export TMPDIR="$fixture/work/tmp" HOME="$fixture/work/home"
cd "$fixture/work/src"
/usr/bin/sandbox-exec -f "$fixture/profile.sb" "$fixture/work/src/probe" job-probe restrictions "$(id -u)" "$fixture/outside"
