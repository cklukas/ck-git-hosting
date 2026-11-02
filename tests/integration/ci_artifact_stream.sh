#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Download a sparse file larger than the historic cap through the real daemon.
set -eu
. "$(dirname "$0")/stop_process.sh"
: "${TMPDIR:?select an isolated test directory}"
: "${CKGIT_HOSTINGD:?select the daemon under test}"
root=$(mktemp -d "$TMPDIR/ci-stream.XXXXXXXX")
server_pid=''
slow_pid=''
download_pid=''
cleanup() {
  stop_process "$download_pid" || true
  stop_process "$slow_pid" || true
  stop_process "$server_pid" || true
  rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM
fail() { echo "artifact stream: $*" >&2; exit 1; }
mkdir "$root/repos" "$root/state"
chmod 0700 "$root/state"
git -C "$root/repos" -c init.defaultBranch=main init --bare -q demo.git
run=00000000000000000001-aaaaaaaa
artifacts="$root/state/ci/runs/demo/$run/artifacts"
mkdir -p "$artifacts"
chmod 0700 "$root/state/ci" "$root/state/ci/runs" "$root/state/ci/runs/demo" "$root/state/ci/runs/demo/$run" "$artifacts"
python3 - "$artifacts/large.tar" <<'PYTEST'
import os, sys
with open(sys.argv[1], 'wb') as f:
    f.truncate((513 << 20) + 7)
os.chmod(sys.argv[1], 0o600)
PYTEST
"$CKGIT_HOSTINGD" --repo-root "$root/repos" --state-root "$root/state" \
  --control-socket "$root/control.sock" --http-port 0 >"$root/server.log" 2>&1 &
server_pid=$!
port=''
attempt=0
while [ -z "$port" ]; do
  kill -0 "$server_pid" 2>/dev/null || { cat "$root/server.log" >&2; fail 'server exited'; }
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$root/server.log")
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail 'server startup timeout'
  [ -n "$port" ] || sleep .1
done
base="http://127.0.0.1:$port"
url="$base/project/demo/ci/$run/artifacts/large"
attempt=0
while :; do
  status=$(curl --silent --head -o "$root/headers" -w '%{http_code}' "$url")
  [ "$status" = 200 ] && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail "artifact not ready: $status"
  sleep .1
done
grep -qi '^Content-Length: 537919495' "$root/headers" || fail 'incorrect HEAD content length'
# A stalled or slow download must not monopolize the dashboard.
curl --silent --limit-rate 1m -o /dev/null "$url" & slow_pid=$!
sleep 1
status=$(curl --silent --max-time 5 -o /dev/null -w '%{http_code}' "$base/")
[ "$status" = 200 ] || fail "dashboard unavailable during download: $status"
stop_process "$slow_pid" || true
slow_pid=''
# curl consumes fixed-size buffers. Monitor the daemon so whole-file buffering
# is rejected rather than only checking that a large GET eventually succeeds.
baseline=$(ps -o rss= -p "$server_pid" | tr -d ' ')
curl --fail --silent --max-time 120 -o /dev/null -w '%{size_download}' "$url" >"$root/size" &
download_pid=$!
while kill -0 "$download_pid" 2>/dev/null; do
  rss=$(ps -o rss= -p "$server_pid" | tr -d ' ')
  [ -n "$rss" ] && [ "$rss" -le $((baseline + 131072)) ] || fail "daemon RSS grew from $baseline to $rss KiB"
  sleep .1
done
wait "$download_pid" || fail 'large download failed'
download_pid=''
[ "$(cat "$root/size")" = 537919495 ] || fail 'large download was truncated'
mv "$artifacts/large.tar" "$root/large.tar"
ln -s /etc/passwd "$artifacts/large.tar"
status=$(curl --silent -o /dev/null -w '%{http_code}' "$url")
[ "$status" = 404 ] || fail "symlink artifact was served: $status"
printf 'large artifact streamed with bounded daemon memory; HEAD and concurrent dashboard passed\n'
