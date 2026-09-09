#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -eu
test_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
[ -d "$test_parent" ] || exit 1
case "${TMPDIR:-}" in "$test_parent"/*) ;; *) exit 1 ;; esac
root=$(mktemp -d "$test_parent/ckd.XXXXXX")
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi
  rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$root/tmp" "$root/bin" "$root/repos" "$root/state" "$root/one" "$root/two" "$root/dest" "$root/collision"
chmod 0700 "$root/state"
TMPDIR="$root/tmp"
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_TERMINAL_PROMPT=0
TEST_ROOT="$root"
CKGIT_TEST_CLIENT_ID=device-one
export TMPDIR GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_TERMINAL_PROMPT TEST_ROOT CKGIT_TEST_CLIENT_ID
unset GIT_SSH GIT_SSH_COMMAND
fail() { echo "Discovery: $*" >&2; exit 1; }
contains() { case "$output" in *"$1"*) ;; *) fail "expected '$1': $output" ;; esac; }
capture() {
  set +e
  "$@" >"$root/out" 2>"$root/err"
  status=$?
  set -e
  output=$(cat "$root/out" "$root/err")
}
ok() { [ "$status" -eq 0 ] || fail "exit $status: $output"; }
client() { "$CKGIT" --config "$root/one/client.ini" "$@"; }
second() { CKGIT_TEST_CLIENT_ID=device-two "$CKGIT" --config "$root/two/client.ini" "$@"; }
for device in one two; do
  printf '%s\n' schema_version=1 "client_id=device-$device" "display_name=$device" \
    server=ckgit@fixture remote_name=ckgit public_path_mode=basename >"$root/$device/client.ini"
done
"$CKGIT_HOSTINGD" --repo-root "$root/repos" --state-root "$root/state" \
  --control-socket "$root/control.sock" --http-port 0 >"$root/server.log" 2>&1 &
server_pid=$!
attempt=0
while [ ! -S "$root/control.sock" ]; do
  kill -0 "$server_pid" || fail 'server failed'
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail 'server timeout'
  sleep .1
done
cat >"$root/bin/ssh" <<'SH'
#!/bin/sh
for argument do request=$argument; done
case "$request" in
  'ckgit-rpc 1 forget-checkout '* ) [ ! -f "$TEST_ROOT/fail-forget" ] || exit 89 ;;
  *git-upload-pack*beta.git* ) [ ! -f "$TEST_ROOT/fail-beta" ] || exit 88 ;;
esac
SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id "$CKGIT_TEST_CLIENT_ID" \
  --repo-root "$TEST_ROOT/repos" --state-root "$TEST_ROOT/state" --control-socket "$TEST_ROOT/control.sock"
SH
chmod 0700 "$root/bin/ssh"
PATH="$root/bin:$PATH"
export PATH
git init -q --initial-branch=main "$root/source"
git -C "$root/source" config user.email tests@example.test
git -C "$root/source" config user.name Tests
printf 'preserved\n' >"$root/source/file.txt"
git -C "$root/source" add file.txt
git -C "$root/source" -c commit.gpgSign=false commit -qm initial
git -C "$root/source" branch feature
git -C "$root/source" -c tag.gpgSign=false tag -a release -m release
for name in alpha beta collision; do
  "$CKGIT_ADMIN" create "$name" --repo-root "$root/repos" >/dev/null
  git -C "$root/source" push -q "$root/repos/$name.git" 'refs/heads/*:refs/heads/*' 'refs/tags/*:refs/tags/*'
done
"$CKGIT_ADMIN" create empty --repo-root "$root/repos" >/dev/null
capture client projects --json
ok
contains '"schema_version":1'
contains '"name":"alpha","state":"unmanaged","checkout":null,"clone_url":"ckgit@fixture:alpha.git"'
capture client projects --filter beta
ok
contains '1 project(s)'
capture client clone --all --into "$root/dest" --dry-run
ok
[ ! -e "$root/dest/alpha" ] && [ ! -e "$root/one/canonical.ini" ] || fail 'preview wrote data'
capture client clone --all --into "$root/dest"
ok
contains 'Preview only'
[ ! -e "$root/dest/alpha" ] || fail 'noninteractive clone bypassed confirmation'
mkdir "$root/dest/collision"
printf keep >"$root/dest/collision/keep"
touch "$root/fail-beta"
capture client clone --all --into "$root/dest" --yes
[ "$status" -eq 3 ] || fail "partial bulk clone should return 3: $output"
contains '2 cloned'
[ -d "$root/dest/alpha/.git" ] && [ -d "$root/dest/empty/.git" ] || fail 'bulk clone did not continue after failure'
[ "$(cat "$root/dest/collision/keep")" = keep ] || fail 'collision was overwritten'
git -C "$root/dest/alpha" show-ref --verify --quiet refs/remotes/ckgit/feature || fail 'branch was not cloned'
git -C "$root/dest/alpha" show-ref --verify --quiet refs/tags/release || fail 'annotated tag was not cloned'
capture client projects --uncloned
ok
contains '2 project(s)'
index_before=$(git -C "$root/dest/alpha" hash-object .git/index)
touch "$root/dest/alpha/file.txt"
capture client projects --json
ok
[ "$(git -C "$root/dest/alpha" hash-object .git/index)" = "$index_before" ] || fail 'project listing rewrote Git index'
rm "$root/fail-beta"
capture client clone --project beta --project beta --into "$root/dest" --yes
ok
contains '1 cloned'
capture client clone --project alpha --project beta --project empty --into "$root/dest" --yes
ok
contains 'No projects to clone; 3 skipped'
# A remaining directory is not an available checkout if its pairing changed.
beta_remote=$(git -C "$root/dest/beta" remote get-url ckgit)
git -C "$root/dest/beta" remote set-url ckgit ckgit@another-server:beta.git
capture client projects --json --filter beta
ok
contains '"name":"beta","state":"invalid"'
contains 'matching configured Git remote'
capture client clone --project beta --into "$root/dest" --yes
[ "$status" -eq 3 ] || fail "invalid managed checkout must need attention: $output"
contains 'Repair its remote'
git -C "$root/dest/beta" remote set-url ckgit "$beta_remote"
# A second device receives its own registration, retained by forgetting on one.
capture second clone alpha "$root/second-alpha"
ok
before_head=$(git -C "$root/dest/alpha" rev-parse HEAD)
before_config=$(git -C "$root/dest/alpha" config --local --list)
printf 'uncommitted\n' >>"$root/dest/alpha/file.txt"
capture client checkout forget alpha --dry-run
ok
[ -f "$root/state/checkouts/alpha/device-one.ini" ] || fail 'forget preview removed record'
touch "$root/fail-forget"
capture client checkout forget alpha --yes
[ "$status" -eq 1 ] || fail 'failed remote forgetting was not reported'
contains 'local selection retained'
case "$(cat "$root/one/canonical.ini")" in *alpha=*) ;; *) fail 'failed forget lost local inventory' ;; esac
rm "$root/fail-forget"
capture client checkout forget alpha --yes
ok
contains 'Forgot alpha'
[ ! -e "$root/state/checkouts/alpha/device-one.ini" ] || fail 'registration survived forgetting'
[ -f "$root/state/checkouts/alpha/device-two.ini" ] || fail 'forget erased another device'
[ "$(git -C "$root/dest/alpha" rev-parse HEAD)" = "$before_head" ] || fail 'forget altered HEAD'
[ "$(git -C "$root/dest/alpha" config --local --list)" = "$before_config" ] || fail 'forget altered Git configuration'
case "$(cat "$root/dest/alpha/file.txt")" in *uncommitted*) ;; *) fail 'forget erased local edits' ;; esac
capture client checkout list
ok
case "$output" in *alpha*) fail "forgotten project remained managed: $output" ;; esac
contains beta
capture client checkout forget alpha --yes
ok
contains 'already unmanaged'
# Missing folders are forgettable, and other selected projects remain managed.
mv "$root/dest/beta" "$root/beta-moved"
capture client checkout forget beta --yes
ok
capture client checkout list
ok
contains empty
case "$output" in *beta*) fail 'missing checkout remained selected' ;; esac
# Option-looking valid names retain their positional boundary.
"$CKGIT_ADMIN" create --config --repo-root "$root/repos" >/dev/null
capture client clone --project=--config --into "$root/dest" --yes
ok
capture client checkout forget --yes -- --config
ok
[ -d "$root/dest/--config/.git" ] || fail 'forget deleted an option-looking checkout'
echo 'Discovery and forgetting integration passed'
