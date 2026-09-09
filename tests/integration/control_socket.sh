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
clone_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" clone \
  --config "$test_root/client.ini" alpha "$test_root/clone")
[ "$(git -C "$test_root/clone" remote)" = 'ckgit' ]
[ "$(git -C "$test_root/clone" rev-parse --abbrev-ref HEAD)" = 'main' ]
case "$clone_output" in
  *"secondary copy"*"$test_root/work"*"checkout set-canonical"*) ;;
  *) echo "clone did not explain the protected existing main checkout: $clone_output" >&2; exit 1 ;;
esac
grep -Fq "alpha=$test_root/work" "$test_root/canonical.ini"
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
  *'Would create and push to ckgit@rpi4:published.git'*'default branch: main (new project)'*'new on server: 1 (refs/heads/main)'*'uncommitted content excluded: 0'*'main checkout on this device: manage '*'Run again with --yes to continue.'*) ;;
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
# Push notifications rebuild the in-memory snapshot asynchronously.
index_attempt=0
while :; do
  curl --max-time 3 --silent --show-error -o "$test_root/published-project.body" \
    "http://127.0.0.1:$http_port/project/published"
  if grep -q '<dt>Last commit across published refs</dt><dd>20' "$test_root/published-project.body"; then break; fi
  index_attempt=$((index_attempt + 1))
  [ "$index_attempt" -lt 30 ] || { echo "Published project was not indexed" >&2; exit 1; }
  sleep 0.1
done
grep -q '<strong>mac-studio</strong> · <code>publish-work</code>' "$test_root/published-project.body"
grep -q 'Project created · <strong>mac-studio</strong>' "$test_root/published-project.body"
grep -q 'Checkout registered · <strong>mac-studio</strong>' "$test_root/published-project.body"
grep -q 'Git push · <strong>mac-studio</strong>' "$test_root/published-project.body"
grep -q '<th>Last commit</th>' "$test_root/published-project.body" || \
  grep -q '<dt>Last commit across published refs</dt>' "$test_root/published-project.body"
grep -q '<dt>Last commit across published refs</dt><dd>20' "$test_root/published-project.body"
curl --max-time 3 --silent --show-error -o "$test_root/table.body" "http://127.0.0.1:$http_port/"
grep -q '>Last commit</th>' "$test_root/table.body"

# The server hands this host back its own registrations.
checkouts=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 checkouts' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
case "$checkouts" in
  "ok 2
alpha 776f726b
published "*) ;;
  *)
    echo "checkout listing did not return this host's registrations: $checkouts" >&2
    exit 1
    ;;
esac
other_host=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 checkouts' "$CK_GIT_SHELL" \
  --client-id laptop --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$other_host" = 'ok 0' ]

# One main checkout per project and host: a second folder is refused until overridden.
git clone -q "$test_root/published.git" "$test_root/second-copy"
git -C "$test_root/second-copy" remote rename origin ckgit
git -C "$test_root/second-copy" remote set-url ckgit 'ckgit@rpi4:published.git'
set +e
second_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name published --yes "$test_root/second-copy" 2>&1)
second_status=$?
set -e
[ "$second_status" -eq 1 ]
case "$second_output" in
  *"published already has a main checkout on this device: $test_root/publish-work"*"--replace-checkout"*) ;;
  *)
    echo "second checkout was not refused with guidance: $second_output" >&2
    exit 1
    ;;
esac
set +e
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/second-copy" >/dev/null 2>"$test_root/second-sync.err"
second_sync_status=$?
set -e
[ "$second_sync_status" -eq 1 ]
grep -Fq "already has a main checkout on this device: $test_root/publish-work" "$test_root/second-sync.err"
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/second-copy" --replace-checkout >/dev/null
grep -Fq "published=$test_root/second-copy" "$test_root/canonical.ini"
curl --max-time 3 --silent --show-error -o "$test_root/replaced-project.body" \
  "http://127.0.0.1:$http_port/project/published"
grep -q '<strong>mac-studio</strong> · <code>second-copy</code>' "$test_root/replaced-project.body"
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --repo "$test_root/publish-work" --replace-checkout >/dev/null
grep -Fq "published=$test_root/publish-work" "$test_root/canonical.ini"

# The private inventory resolves both managed folders without scan roots, even
# while public reports expose only basenames. Configurations in this directory
# belong to the same device/server and use the same private selection file.
printf '%s\n' \
  'schema_version=1' \
  'client_id=mac-studio' \
  'display_name=Mac Studio' \
  'server=ckgit@rpi4' \
  'remote_name=ckgit' \
  'public_path_mode=basename' >"$test_root/rootless.ini"
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" register \
  --config "$test_root/rootless.ini" --repo "$test_root/publish-work" --replace-checkout >/dev/null
git -C "$test_root/publish-work" commit -q --allow-empty -m registered-sync
registered_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/rootless.ini" 2>&1)
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = \
  "$(git -C "$test_root/publish-work" rev-parse HEAD)" ]
case "$registered_output" in
  *"Scope: 2 managed project(s)"*"Synced $test_root/work"*"Synced $test_root/publish-work"*) ;;
  *)
    echo "rootless sync did not use both private managed paths: $registered_output" >&2
    exit 1
    ;;
esac
case "$registered_output" in
  *"unavailable:"*)
    echo "basename privacy made a managed checkout unavailable: $registered_output" >&2
    exit 1
    ;;
esac

# Publish scope flags: a limited push leaves other branches and tags out.
git init -q --initial-branch=main "$test_root/scoped"
git -C "$test_root/scoped" config user.email 'tests@example.test'
git -C "$test_root/scoped" config user.name 'Tests'
git -C "$test_root/scoped" commit -q --allow-empty -m initial
git -C "$test_root/scoped" branch experiment
git -C "$test_root/scoped" tag v1
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped --branch main --no-tags --yes "$test_root/scoped" >/dev/null
[ "$(git --git-dir "$test_root/scoped.git" for-each-ref --format='%(refname)' | tr '\n' ' ')" = 'refs/heads/main ' ]
if PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped --branch missing --yes "$test_root/scoped" >/dev/null 2>&1; then
  echo "publish accepted a branch limit that does not exist" >&2
  exit 1
fi
scoped_preview=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped "$test_root/scoped")
case "$scoped_preview" in
  *'new on server: 2 (refs/heads/experiment, refs/tags/v1)'*'to update (differing tips; permission checked by Git preflight): 0'*'already up to date: 1'*) ;;
  *)
    echo "publish preview did not list the unpublished branch and tag: $scoped_preview" >&2
    exit 1
    ;;
esac
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped --yes "$test_root/scoped" >/dev/null
[ "$(git --git-dir "$test_root/scoped.git" for-each-ref --format='%(refname)' | sort | tr '\n' ' ')" = \
  'refs/heads/experiment refs/heads/main refs/tags/v1 ' ]
git -C "$test_root/scoped" commit -q --allow-empty -m newer
update_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped --yes "$test_root/scoped")
case "$update_output" in
  *'to update (differing tips; permission checked by Git preflight): 1 (refs/heads/main)'*'Git preflight: ref updates permitted without force'*'Published scoped'*) ;;
  *)
    echo "publish did not update the branch that moved: $update_output" >&2
    exit 1
    ;;
esac
[ "$(git --git-dir "$test_root/scoped.git" rev-parse refs/heads/main)" = "$(git -C "$test_root/scoped" rev-parse HEAD)" ]
idle_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped --yes "$test_root/scoped")
case "$idle_output" in
  *'Everything in scope is already published'*) ;;
  *)
    echo "publish did not recognize an up-to-date project: $idle_output" >&2
    exit 1
    ;;
esac
# A limited first publish makes a pushed branch the default, never the unpushed current one.
git clone -q "$test_root/scoped" "$test_root/scoped-copy"
git -C "$test_root/scoped-copy" remote remove origin
git -C "$test_root/scoped-copy" branch experiment
PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" publish \
  --config "$test_root/client.ini" --name scoped-default --branch experiment --no-tags --yes "$test_root/scoped-copy" >/dev/null
[ "$(git --git-dir "$test_root/scoped-default.git" symbolic-ref --short HEAD)" = 'experiment' ]
[ "$(git --git-dir "$test_root/scoped-default.git" for-each-ref --format='%(refname)' | tr '\n' ' ')" = 'refs/heads/experiment ' ]
if PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/client.ini" --replace-checkout >/dev/null 2>&1; then
  echo "sync accepted --replace-checkout without --repo" >&2
  exit 1
fi

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
  --config "$test_root/sync-all.ini" --scan --dry-run >/dev/null

# Legacy basename registrations need an explicit main selection when discovery
# finds duplicate checkouts. Listing additional copies is an explicit --scan.
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
  --config "$test_root/dup/client.ini" --scan --dry-run 2>&1)
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
  --config "$test_root/dup/client.ini" --scan published 2>/dev/null)
list_status=$?
set -e
[ "$list_status" -eq 3 ]
case "$list_output" in
  *"published
  main checkout on this device: publish-work
  unavailable: legacy registration publish-work needs a local path"*"checkout migrate"*"Additional discovery"*"$test_root/publish-work"*"[additional copy; not selected for ordinary sync]"*"$test_root/publish-work.Ab12Cd [ephemeral name]"*) ;;
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
  --config "$test_root/dup/client.ini" --scan >/dev/null
[ "$(git --git-dir "$test_root/published.git" rev-parse refs/heads/main)" = \
  "$(git -C "$test_root/publish-work" rev-parse HEAD)" ]
list_output=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout list \
  --config "$test_root/dup/client.ini" --scan published)
case "$list_output" in
  *"published
  main       $test_root/publish-work [private local inventory]"*"reported to server: publish-work"*"$test_root/publish-work.Ab12Cd [ephemeral name] [additional copy; not selected for ordinary sync]"*) ;;
  *)
    echo "checkout list did not show the selected canonical checkout: $list_output" >&2
    exit 1
    ;;
esac
default_list=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" checkout list \
  --config "$test_root/dup/client.ini" published)
case "$default_list" in
  *"$test_root/publish-work.Ab12Cd"*)
    echo "default checkout list unexpectedly included an unmanaged discovered copy: $default_list" >&2
    exit 1
    ;;
esac

# Per-client lock: a second mutating sync for the same configuration fails fast.
# Read-only previews deliberately do not create or acquire this lock.
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
  --config "$test_root/dup/client.ini" --repo "$test_root/publish-work" \
  >/dev/null 2>"$test_root/busy.err"
busy_status=$?
wait "$slow_sync_pid"
slow_sync_status=$?
set -e
[ "$busy_status" -eq 4 ]
grep -q 'another ckgit operation for this configuration is running' "$test_root/busy.err"
[ "$slow_sync_status" -eq 0 ]
[ -f "$test_root/dup/sync.lock" ]

# Interrupting the client must also end the child tree it is waiting on.
rm -f "$test_root/lock-marker"
PATH="$test_root/slow-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" sync \
  --config "$test_root/dup/client.ini" --repo "$test_root/publish-work" >/dev/null 2>&1 &
interrupted_pid=$!
attempt=0
while [ ! -e "$test_root/lock-marker" ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 20 ]; then
    echo "interrupt fixture never reached its registration step" >&2
    exit 1
  fi
  sleep 1
done
kill -INT "$interrupted_pid"
set +e
wait "$interrupted_pid"
interrupted_status=$?
set -e
[ "$interrupted_status" -ne 0 ]
sleep 1
if pgrep -f "slow-bin/ssh" >/dev/null 2>&1; then
  echo "child tree survived the client's interruption" >&2
  pkill -f "slow-bin/ssh" || true
  exit 1
fi

# ckgit web: the tunnel is a shell-free ssh child; here a stub "ssh" answers the
# forwarded local port with a second daemon so the readiness probe, URL, and
# Ctrl+C handling are exercised without a network.
mkdir "$test_root/web-bin"
printf '%s\n' \
  '#!/bin/sh' \
  'lport=""' \
  'for argument do case "$argument" in 127.0.0.1:*:127.0.0.1:*) lport=${argument#127.0.0.1:}; lport=${lport%%:*} ;; esac; done' \
  '[ -n "$lport" ] || exit 1' \
  'exec "$CKGIT_HOSTINGD" --repo-root "$TEST_ROOT" --control-socket "$TEST_ROOT/web.sock" --http-port "$lport"' \
  >"$test_root/web-bin/ssh"
chmod 0700 "$test_root/web-bin/ssh"
PATH="$test_root/web-bin:$PATH" TEST_ROOT="$test_root" CKGIT_HOSTINGD="$CKGIT_HOSTINGD" "$CKGIT" web \
  --config "$test_root/client.ini" --no-open --port 18420 >"$test_root/web.out" 2>&1 &
web_pid=$!
attempt=0
while ! grep -q 'Dashboard: http://127.0.0.1:18420/' "$test_root/web.out" 2>/dev/null; do
  if ! kill -0 "$web_pid" 2>/dev/null; then
    cat "$test_root/web.out" >&2
    echo "ckgit web exited before announcing the dashboard URL" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 20 ]; then
    cat "$test_root/web.out" >&2
    echo "ckgit web did not announce the dashboard URL" >&2
    exit 1
  fi
  sleep 1
done
curl --max-time 3 --silent --show-error http://127.0.0.1:18420/ | grep -q '<h1>Projects</h1>'
kill -INT "$web_pid"
set +e
wait "$web_pid"
web_status=$?
set -e
[ "$web_status" -eq 0 ]
grep -q 'Tunnel closed' "$test_root/web.out"
sleep 1
if curl --max-time 2 --silent http://127.0.0.1:18420/ >/dev/null 2>&1; then
  echo "tunnel child kept running after Ctrl+C" >&2
  exit 1
fi
if PATH="$test_root/web-bin:$PATH" TEST_ROOT="$test_root" CKGIT_HOSTINGD="$CKGIT_HOSTINGD" "$CKGIT" web \
  --config "$test_root/client.ini" --no-open --port 18420 --remote-port 1 'bad host' >/dev/null 2>&1; then
  echo "ckgit web accepted an unsafe SSH target" >&2
  exit 1
fi

ping=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 ping' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$ping" = 'ok' ]

projects=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 list-projects' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$projects" = "ok 6
alpha
beta
gamma
published
scoped
scoped-default" ]

# A repository with hundreds of tags must still list and compare its refs.
tag_index=0
while [ "$tag_index" -lt 300 ]; do
  git -C "$test_root/work" tag "v0.$tag_index" >/dev/null
  tag_index=$((tag_index + 1))
done
git -C "$test_root/work" push -q local --tags
many_refs=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 refs alpha' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root" --control-socket "$test_root/control.sock")
[ "$(printf '%s\n' "$many_refs" | head -1)" = 'ok 301' ]
many_status=$(PATH="$test_root/test-bin:$PATH" TEST_ROOT="$test_root" "$CKGIT" status \
  --config "$test_root/client.ini" --repo "$test_root/work")
case "$many_status" in
  *'301 equal, 0 local-only, 0 server-only, 0 mismatched'*) ;;
  *)
    echo "client status did not compare hundreds of refs: $many_status" >&2
    exit 1
    ;;
esac
git -C "$test_root/work" tag -d $(git -C "$test_root/work" tag -l 'v0.*') >/dev/null
git --git-dir "$test_root/alpha.git" tag -d $(git --git-dir "$test_root/alpha.git" tag -l 'v0.*') >/dev/null

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
