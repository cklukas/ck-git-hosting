#!/bin/sh
# Unprivileged check of the actual production policy, not UID supervision.
set -eu
bin=$1
parent=$2
export TMPDIR="$parent"
[ -d "$TMPDIR" ]
fixture=$(mktemp -d "$TMPDIR/native-policy.XXXXXX")
cleanup() {
  for path in "$fixture/work/src/outside-link" "$fixture/work/src/probe" "$fixture/outside" "$fixture/profile.sb"; do
    if [ -e "$path" ] || [ -L "$path" ]; then /bin/rm "$path"; fi
  done
  for dir in "$fixture/work/src" "$fixture/work/tmp" "$fixture/work/home" "$fixture/work" "$fixture"; do
    if [ -d "$dir" ]; then rmdir "$dir"; fi
  done
}
trap cleanup EXIT HUP INT TERM
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
