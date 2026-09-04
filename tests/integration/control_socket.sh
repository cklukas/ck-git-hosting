#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

set -eu

# The Makefile exports the approved build root; on the development Mac that is
# /Volumes/PRO-BLADE/tmp, in CI it is the runner's temporary directory.
test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *)
    echo "TMPDIR must be beneath $test_root_parent" >&2
    exit 1
    ;;
esac

# Unix-domain sockets have a short platform-specific path limit.  Keep this
# fixture directly under the approved temporary root instead of nesting it
# below the build directory.
test_root=$(mktemp -d "$test_root_parent/ckg.XXXXXX")
server_pid=''

cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

"$CKGIT_ADMIN" create alpha --repo-root "$test_root" >/dev/null
"$CKGIT_ADMIN" create beta --repo-root "$test_root" >/dev/null
mkdir "$test_root/state"
chmod 0700 "$test_root/state"
mkdir "$test_root/hooks"
cp "$CKGIT_POST_RECEIVE" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
git init --initial-branch=main -q "$test_root/work"
git -C "$test_root/work" config user.email 'tests@example.test'
git -C "$test_root/work" config user.name 'Tests'
git -C "$test_root/work" commit -q --allow-empty -m initial
git -C "$test_root/work" remote add local "$test_root/alpha.git"
git -C "$test_root/work" push -q local HEAD:refs/heads/main
git -C "$test_root/work" remote add ckgit 'ckgit@rpi4:alpha.git'
printf '%s\n' \
  'schema_version=1' \
  'client_id=mac-studio' \
  'display_name=Mac Studio' \
  'server=ckgit@rpi4' \
  'remote_name=ckgit' \
  "scan_root=$test_root" \
  'exclude=*/alpha*' \
  'public_path_mode=basename' >"$test_root/client.ini"
config_output=$("$CKGIT" config show --config "$test_root/client.ini")
case "$config_output" in
  *'client_id=mac-studio'*'scan_root='*) ;;
  *)
    echo "client configuration was not rendered" >&2
    exit 1
    ;;
esac
scan_output=$("$CKGIT" scan --config "$test_root/client.ini")
case "$scan_output" in
  *'Scanned 1 repository.') ;;
  *)
    echo "configured scan did not find the expected worktree" >&2
    exit 1
    ;;
esac
"$CKGIT_HOSTINGD" --repo-root "$test_root" --control-socket "$test_root/control.sock" \
  --state-root "$test_root/state" --hook-directory "$test_root/hooks" --http-port 0 \
  >"$test_root/server.log" 2>&1 &
server_pid=$!

attempt=0
while [ ! -S "$test_root/control.sock" ]; do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    sed -n '1,80p' "$test_root/server.log" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 20 ]; then
    echo "control socket did not become ready" >&2
    exit 1
  fi
  sleep 1
done

http_port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$test_root/server.log")
[ -n "$http_port" ]
curl --max-time 3 --silent --show-error -D "$test_root/http.headers" -o "$test_root/http.body" \
  "http://127.0.0.1:$http_port/"
grep -q '^HTTP/1.1 200 OK' "$test_root/http.headers"
grep -q "Content-Security-Policy: default-src 'none'" "$test_root/http.headers"
grep -q '<h1>Projects</h1>' "$test_root/http.body"
grep -q 'href="/project/alpha">alpha</a>' "$test_root/http.body"
curl --max-time 3 --silent --show-error -o "$test_root/project.body" \
  "http://127.0.0.1:$http_port/project/alpha"
grep -q '<h1>alpha</h1>' "$test_root/project.body"
head_headers=$(curl --max-time 3 --silent --show-error --head "http://127.0.0.1:$http_port/project/alpha")
case "$head_headers" in
  *'HTTP/1.1 200 OK'*'Content-Length:'*) ;;
  *)
    echo "HTTP HEAD response was not valid" >&2
    exit 1
    ;;
esac
if printf 'not-a-git-update\n' | CKGIT_STATE_ROOT="$test_root/state" CKGIT_CLIENT_ID=mac-studio \
  CKGIT_PROJECT_NAME=alpha "$CKGIT_POST_RECEIVE" >/dev/null 2>&1; then
  echo "post-receive hook accepted malformed ref input" >&2
  exit 1
fi
bad_request=$(printf 'GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n' | \
  nc -w 3 127.0.0.1 "$http_port" || true)
case "$bad_request" in
  'HTTP/1.1 400 Bad Request'*) ;;
  *)
    echo "daemon accepted an ambiguous HTTP request" >&2
    exit 1
    ;;
esac

mkdir "$test_root/test-bin"
printf '%s\n' \
  '#!/bin/sh' \
  'for argument do request=$argument; done' \
  'SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id mac-studio --repo-root "$TEST_ROOT" --control-socket "$TEST_ROOT/control.sock" --state-root "$TEST_ROOT/state"' \
  >"$test_root/test-bin/ssh"
chmod 0700 "$test_root/test-bin/ssh"
status_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" status \
  --config "$test_root/client.ini" --repo "$test_root/work")
case "$status_output" in
  *'project: alpha'*'1 equal, 0 local-only, 0 server-only, 0 mismatched'*) ;;
  *)
    echo "client status did not report equal paired refs" >&2
    exit 1
    ;;
esac
registration_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" register \
  --config "$test_root/client.ini" --repo "$test_root/work")
case "$registration_output" in
  *'Registered alpha from work'*) ;;
  *)
    echo "client did not register its paired checkout" >&2
    exit 1
    ;;
esac
curl --max-time 3 --silent --show-error -o "$test_root/registered-project.body" \
  "http://127.0.0.1:$http_port/project/alpha"
grep -q '<strong>mac-studio</strong> · <code>work</code>' "$test_root/registered-project.body"
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" clone \
  --config "$test_root/client.ini" alpha "$test_root/clone" >/dev/null
[ "$(git -C "$test_root/clone" remote)" = 'ckgit' ]
[ "$(git -C "$test_root/clone" rev-parse --abbrev-ref HEAD)" = 'main' ]
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" create \
  --config "$test_root/client.ini" gamma --default-branch main >/dev/null
[ "$(git --git-dir "$test_root/gamma.git" config --get receive.denyDeletes)" = 'true' ]
git init --initial-branch=main -q "$test_root/publish-work"
git -C "$test_root/publish-work" config user.email 'tests@example.test'
git -C "$test_root/publish-work" config user.name 'Tests'
git -C "$test_root/publish-work" commit -q --allow-empty -m initial
preview=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name published "$test_root/publish-work")
case "$preview" in
  *'Would create and push to ckgit@rpi4:published.git'*'Run again with --yes to continue.'*) ;;
  *)
    echo "publish did not present the expected preview" >&2
    exit 1
    ;;
esac
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name published --yes "$test_root/publish-work" >/dev/null
[ "$(git -C "$test_root/publish-work" remote)" = 'ckgit' ]
[ "$(git --git-dir "$test_root/published.git" config --get core.hooksPath)" = "$test_root/hooks" ]
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = \
  "$(git -C "$test_root/publish-work" rev-parse HEAD)" ]
curl --max-time 3 --silent --show-error -o "$test_root/published-project.body" \
  "http://127.0.0.1:$http_port/project/published"
grep -q '<strong>mac-studio</strong> · <code>publish-work</code>' "$test_root/published-project.body"
grep -q 'Project created · <strong>mac-studio</strong>' "$test_root/published-project.body"
grep -q 'Checkout registered · <strong>mac-studio</strong>' "$test_root/published-project.body"
grep -q 'Git push · <strong>mac-studio</strong>' "$test_root/published-project.body"
git -C "$test_root/publish-work" commit -q --allow-empty -m follow-up
before_sync=$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/publish-work" --dry-run >/dev/null
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = "$before_sync" ]
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/publish-work" >/dev/null
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = \
  "$(git -C "$test_root/publish-work" rev-parse HEAD)" ]
blocked_registration_tip=$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)
git -C "$test_root/publish-work" commit -q --allow-empty -m blocked-registration
chmod 0500 "$test_root/state/checkouts/published"
set +e
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/publish-work" >/dev/null 2>&1
blocked_registration_status=$?
set -e
chmod 0700 "$test_root/state/checkouts/published"
[ "$blocked_registration_status" -eq 3 ]
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = "$blocked_registration_tip" ]
printf '%s\n' \
  'schema_version=1' \
  'client_id=mac-studio' \
  'display_name=Mac Studio' \
  'server=ckgit@rpi4' \
  'remote_name=ckgit' \
  "scan_root=$test_root/publish-work" >"$test_root/sync-all.ini"
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/sync-all.ini" --dry-run >/dev/null

# Duplicate checkouts of one project: automatic sync needs a canonical selection.
git clone -q "$test_root/published.git" "$test_root/publish-work.Ab12Cd"
git -C "$test_root/publish-work.Ab12Cd" remote rename origin ckgit
git -C "$test_root/publish-work.Ab12Cd" remote set-url ckgit 'ckgit@rpi4:published.git'
mkdir "$test_root/dup"
printf '%s\n' \
  'schema_version=1' \
  'client_id=mac-studio' \
  'display_name=Mac Studio' \
  'server=ckgit@rpi4' \
  'remote_name=ckgit' \
  "scan_root=$test_root/publish-work" \
  "scan_root=$test_root/publish-work.Ab12Cd" >"$test_root/dup/client.ini"
set +e
duplicate_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/dup/client.ini" --dry-run 2>&1)
duplicate_status=$?
set -e
[ "$duplicate_status" -eq 3 ]
case "$duplicate_output" in
  *"published: duplicate checkouts; automatic sync skipped"*"(proposed: $test_root/publish-work)"*) ;;
  *)
    echo "duplicate checkouts were not skipped with a proposal: $duplicate_output" >&2
    exit 1
    ;;
esac
set +e
list_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout list \
  --config "$test_root/dup/client.ini" published 2>/dev/null)
list_status=$?
set -e
[ "$list_status" -eq 3 ]
case "$list_output" in
  "published
  proposed   $test_root/publish-work
  other      $test_root/publish-work.Ab12Cd [ephemeral name]
  note: no canonical checkout is selected; automatic sync skips this project") ;;
  *)
    echo "checkout list did not describe the duplicate checkouts: $list_output" >&2
    exit 1
    ;;
esac
if PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout set-canonical \
  --config "$test_root/dup/client.ini" alpha "$test_root/publish-work" >/dev/null 2>&1; then
  echo "set-canonical accepted a checkout that is not paired with the named project" >&2
  exit 1
fi
[ ! -e "$test_root/dup/canonical.ini" ]
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout set-canonical \
  --config "$test_root/dup/client.ini" published "$test_root/publish-work" >/dev/null
grep -q "^published=$test_root/publish-work\$" "$test_root/dup/canonical.ini"
git -C "$test_root/publish-work" commit -q --allow-empty -m canonical-sync
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/dup/client.ini" >/dev/null
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = \
  "$(git -C "$test_root/publish-work" rev-parse HEAD)" ]
list_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout list \
  --config "$test_root/dup/client.ini")
case "$list_output" in
  "published
  canonical  $test_root/publish-work
  other      $test_root/publish-work.Ab12Cd [ephemeral name]") ;;
  *)
    echo "checkout list did not show the selected canonical checkout: $list_output" >&2
    exit 1
    ;;
esac

# Per-client lock: a second sync or publish for the same configuration fails fast.
mkdir "$test_root/slow-bin"
printf '%s\n' \
  '#!/bin/sh' \
  'for argument do request=$argument; done' \
  'case "$request" in *register*) : >"$TEST_ROOT/lock-marker"; sleep 3 ;; esac' \
  'SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id mac-studio --repo-root "$TEST_ROOT" --control-socket "$TEST_ROOT/control.sock" --state-root "$TEST_ROOT/state"' \
  >"$test_root/slow-bin/ssh"
chmod 0700 "$test_root/slow-bin/ssh"
PATH="$test_root/slow-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/dup/client.ini" --repo "$test_root/publish-work" >/dev/null 2>&1 &
slow_sync_pid=$!
attempt=0
while [ ! -e "$test_root/lock-marker" ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 20 ]; then
    echo "background sync never reached its registration step" >&2
    exit 1
  fi
  sleep 1
done
set +e
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/dup/client.ini" --repo "$test_root/publish-work" --dry-run \
  >/dev/null 2>"$test_root/busy.err"
busy_status=$?
wait "$slow_sync_pid"
slow_sync_status=$?
set -e
[ "$busy_status" -eq 4 ]
grep -q 'another sync or publish for this configuration is running' "$test_root/busy.err"
[ "$slow_sync_status" -eq 0 ]
[ -f "$test_root/dup/sync.lock" ]

ping=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 ping' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$ping" = 'ok' ]

projects=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 list-projects' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$projects" = "ok 4
alpha
beta
gamma
published" ]

refs=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 refs alpha' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
case "$refs" in
  "ok 1
refs/heads/main "*) ;;
  *)
    echo "control ref response did not contain the pushed branch" >&2
    exit 1
    ;;
esac

if SSH_ORIGINAL_COMMAND="git-upload-pack '../alpha.git'" "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock" \
  --dry-run >/dev/null 2>&1; then
  echo "dispatcher accepted a traversal repository argument" >&2
  exit 1
fi

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=''

echo "control socket integration test passed"
