#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
[ -d "$test_root_parent" ]
# Keep the fixture socket path short enough for macOS and Linux.
test_root=$(mktemp -d "$test_root_parent/ckb.XXXXXX")
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$test_root/tmp"
TMPDIR="$test_root/tmp"
export TMPDIR
# Exercise fixture Git and SSH independently of the user's configuration.
unset GIT_SSH GIT_SSH_COMMAND
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_TERMINAL_PROMPT=0
export GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_TERMINAL_PROMPT

fail() {
  echo "bulk publish: $*" >&2
  exit 1
}
contains() {
  case "$1" in
    *"$2"*) ;;
    *) fail "expected output to contain '$2': $1" ;;
  esac
}
new_repo() {
  git init --initial-branch=main -q "$1"
  git -C "$1" config user.email 'tests@example.test'
  git -C "$1" config user.name 'Tests'
  git -C "$1" commit -q --allow-empty -m initial
}
assert_no_remote() {
  [ -z "$(git -C "$1" remote)" ] || fail "preview or skipped repository gained a remote: $1"
}
assert_same_ref() {
  [ "$(git -C "$1" rev-parse "$3")" = "$(git --git-dir "$test_root/repos/$2.git" rev-parse "$3")" ] ||
    fail "published ref differs: $2 $3"
}
assert_absent_ref() {
  if git --git-dir "$test_root/repos/$1.git" show-ref --verify --quiet "$2"; then
    fail "excluded ref was published: $1 $2"
  fi
}

mkdir "$test_root/repos" "$test_root/state" "$test_root/hooks" "$test_root/test-bin"
chmod 0700 "$test_root/state"
cp "$CKGIT_POST_RECEIVE" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
"$CKGIT_ADMIN" create collision --repo-root "$test_root/repos" >/dev/null
"$CKGIT_ADMIN" create paired --repo-root "$test_root/repos" >/dev/null
printf '%s\n' \
  'schema_version=1' \
  'client_id=bulk-test' \
  'display_name=Bulk Test' \
  'server=ckgit@fixture' \
  'remote_name=ckgit' \
  'public_path_mode=full' >"$test_root/client.ini"
"$CKGIT_HOSTINGD" --repo-root "$test_root/repos" --control-socket "$test_root/control.sock" \
  --state-root "$test_root/state" --hook-directory "$test_root/hooks" --http-port 0 \
  >"$test_root/server.log" 2>&1 &
server_pid=$!
attempt=0
while [ ! -S "$test_root/control.sock" ]; do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server.log" >&2
    fail 'fixture daemon exited before becoming ready'
  fi
  attempt=$((attempt + 1))
  [ "$attempt" -lt 20 ] || fail 'fixture control socket did not become ready'
  sleep 1
done

cat >"$test_root/test-bin/ssh" <<'SH'
#!/bin/sh
for argument do request=$argument; done
if [ -n "${CKGIT_TEST_REJECT_PUSH:-}" ]; then
  case "$request" in
    *git-receive-pack*"$CKGIT_TEST_REJECT_PUSH.git"*)
      echo 'fixture rejected this push' >&2
      exit 1
      ;;
  esac
fi
SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id bulk-test \
  --repo-root "$TEST_ROOT/repos" --control-socket "$TEST_ROOT/control.sock" --state-root "$TEST_ROOT/state"
SH
chmod 0700 "$test_root/test-bin/ssh"
PATH="$test_root/test-bin:$PATH"
TEST_ROOT="$test_root"
export PATH TEST_ROOT
publish() {
  "$CKGIT" publish --config "$test_root/client.ini" "$@"
}

# Only immediate real worktrees are candidates. Spaces in the parent path must
# survive discovery, the SSH push, and checkout registration without shell splits.
parent="$test_root/projects with spaces"
mkdir "$parent"
new_repo "$parent/alpha"
git -C "$parent/alpha" branch feature/extra
git -C "$parent/alpha" tag -a v1 -m 'annotated release'
git -C "$parent/alpha" tag lightweight
new_repo "$parent/beta"
new_repo "$parent/paired-copy"
git -C "$parent/paired-copy" remote add ckgit 'ckgit@fixture:paired.git'
new_repo "$parent/collision"
mkdir "$parent/container" "$parent/documents"
new_repo "$parent/container/deeper"
new_repo "$test_root/external"
ln -s "$test_root/external" "$parent/linked"
git init --bare -q "$parent/bare.git"

preview=$(publish --dry-run "$parent" 2>&1)
contains "$preview" alpha
contains "$preview" beta
contains "$preview" 'projects with spaces'
contains "$preview" 'refs/heads/feature/extra'
contains "$preview" 'refs/heads/main'
contains "$preview" 'refs/tags/lightweight'
contains "$preview" 'refs/tags/v1'
contains "$preview" 'default branch: main (new project)'
contains "$preview" 'main checkout on this device: manage '
contains "$preview" 'uncommitted content excluded: 0'
[ ! -e "$test_root/repos/alpha.git" ] || fail 'dry run created a project'
[ ! -e "$test_root/repos/beta.git" ] || fail 'dry run created a project'
[ ! -e "$test_root/canonical.ini" ] || fail 'dry run changed private checkout management'
assert_no_remote "$parent/alpha"
assert_no_remote "$parent/beta"
assert_no_remote "$parent/collision"
# --dry-run always wins over --yes, and redirected input never authorizes a push.
publish --yes --dry-run "$parent" >"$test_root/dry-run-yes.log" 2>&1
preview=$(publish "$parent" </dev/null 2>&1)
contains "$preview" '--yes'
[ ! -e "$test_root/repos/alpha.git" ] || fail 'nonterminal preview created a project'
[ ! -e "$test_root/canonical.ini" ] || fail 'nonterminal preview changed private checkout management'
assert_no_remote "$parent/alpha"

# Folder mode cannot reuse one name or replace existing checkout registrations.
set +e
publish --name renamed --yes "$parent" >"$test_root/name.log" 2>&1
name_status=$?
publish --replace-checkout --yes "$parent" >"$test_root/replace.log" 2>&1
replace_status=$?
set -e
[ "$name_status" -eq 2 ] || fail "folder --name returned $name_status instead of usage error"
[ "$replace_status" -eq 2 ] || fail "folder --replace-checkout returned $replace_status instead of usage error"
[ ! -e "$test_root/repos/renamed.git" ] || fail 'invalid folder options mutated the server'

# No path argument selects the current folder. All local branches and tags are
# sent by default, while existing project names are left paired only where they were.
(cd "$parent" && publish --yes) >"$test_root/published.log" 2>&1 || {
  cat "$test_root/published.log" >&2
  fail 'publishing the candidate folder failed'
}
assert_same_ref "$parent/alpha" alpha refs/heads/main
assert_same_ref "$parent/alpha" alpha refs/heads/feature/extra
assert_same_ref "$parent/alpha" alpha refs/tags/v1
assert_same_ref "$parent/alpha" alpha refs/tags/lightweight
assert_same_ref "$parent/beta" beta refs/heads/main
[ "$(git -C "$parent/alpha" remote get-url ckgit)" = 'ckgit@fixture:alpha.git' ]
[ "$(git -C "$parent/paired-copy" remote get-url ckgit)" = 'ckgit@fixture:paired.git' ]
assert_no_remote "$parent/collision"
assert_absent_ref collision refs/heads/main
assert_absent_ref paired refs/heads/main
for ignored in deeper linked external bare.git paired-copy; do
  [ ! -e "$test_root/repos/$ignored.git" ] || fail "noncandidate was published: $ignored"
done
assert_no_remote "$parent/container/deeper"
assert_no_remote "$test_root/external"
checkouts=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 checkouts' "$CK_GIT_SHELL" \
  --client-id bulk-test --repo-root "$test_root/repos" --control-socket "$test_root/control.sock")
contains "$checkouts" 'alpha '
contains "$checkouts" 'beta '
grep -Fq "alpha=$parent/alpha" "$test_root/canonical.ini" || fail 'alpha was not added to private management'
grep -Fq "beta=$parent/beta" "$test_root/canonical.ini" || fail 'beta was not added to private management'

# A repeat is a successful no-op. Calling from inside a checkout still retains
# single-project behavior, including the new explicit preview spelling.
publish --yes "$parent" >"$test_root/repeat.log" 2>&1 || fail 'repeat folder publish failed'
assert_no_remote "$parent/collision"
mkdir "$parent/alpha/subdirectory"
single_preview=$(publish --dry-run "$parent/alpha/subdirectory" 2>&1)
contains "$single_preview" 'Would push to ckgit@fixture:alpha.git'

# A missing requested branch skips that repository without preventing another
# candidate from publishing. The selected branch becomes the server default.
scoped="$test_root/scoped"
mkdir "$scoped"
new_repo "$scoped/a-missing"
new_repo "$scoped/z-scoped"
git -C "$scoped/z-scoped" branch release
git -C "$scoped/z-scoped" tag v2
set +e
publish --yes --branch release --no-tags "$scoped" >"$test_root/scoped.log" 2>&1
scoped_status=$?
set -e
[ "$scoped_status" -eq 3 ] || fail "partial scoped batch returned $scoped_status instead of 3"
[ ! -e "$test_root/repos/a-missing.git" ] || fail 'missing-branch repository was created'
assert_no_remote "$scoped/a-missing"
assert_same_ref "$scoped/z-scoped" z-scoped refs/heads/release
assert_absent_ref z-scoped refs/heads/main
assert_absent_ref z-scoped refs/tags/v2
[ "$(git --git-dir "$test_root/repos/z-scoped.git" symbolic-ref HEAD)" = refs/heads/release ]

# Existing committed branches do not make an unborn checked-out branch valid.
# The invalid candidate is skipped before creating its server project or remote.
unborn="$test_root/unborn"
mkdir "$unborn"
new_repo "$unborn/a-unborn"
git -C "$unborn/a-unborn" checkout --orphan unborn -q
new_repo "$unborn/z-after-unborn"
set +e
publish --yes "$unborn" >"$test_root/unborn.log" 2>&1
unborn_status=$?
set -e
[ "$unborn_status" -eq 3 ] || fail "unborn branch batch returned $unborn_status instead of 3"
[ ! -e "$test_root/repos/a-unborn.git" ] || fail 'unborn default branch project was created'
assert_no_remote "$unborn/a-unborn"
assert_same_ref "$unborn/z-after-unborn" z-after-unborn refs/heads/main

# A ckgit remote targeting another server is never overwritten. It is reported
# as a skipped failure, and a later valid candidate can still publish.
conflicting="$test_root/conflicting"
mkdir "$conflicting"
new_repo "$conflicting/a-conflict"
git -C "$conflicting/a-conflict" remote add ckgit 'https://example.invalid/project.git'
new_repo "$conflicting/z-unpaired"
set +e
publish --yes "$conflicting" >"$test_root/conflicting.log" 2>&1
conflict_status=$?
set -e
[ "$conflict_status" -eq 3 ] || fail "conflicting remote batch returned $conflict_status instead of 3"
[ "$(git -C "$conflicting/a-conflict" remote get-url ckgit)" = 'https://example.invalid/project.git' ]
[ ! -e "$test_root/repos/a-conflict.git" ] || fail 'conflicting remote repository was published'
assert_same_ref "$conflicting/z-unpaired" z-unpaired refs/heads/main

# A transport failure after creation must not abort the remaining candidates.
failing="$test_root/failing"
mkdir "$failing"
new_repo "$failing/a-rejected"
new_repo "$failing/z-after-failure"
CKGIT_TEST_REJECT_PUSH=a-rejected
export CKGIT_TEST_REJECT_PUSH
set +e
publish --yes "$failing" >"$test_root/failing.log" 2>&1
failure_status=$?
set -e
unset CKGIT_TEST_REJECT_PUSH
[ "$failure_status" -eq 3 ] || fail "failed push batch returned $failure_status instead of 3"
[ -d "$test_root/repos/a-rejected.git" ] || fail 'injected push failure did not reach creation'
assert_absent_ref a-rejected refs/heads/main
assert_same_ref "$failing/z-after-failure" z-after-failure refs/heads/main

# Folder mode skips paired repositories, so recovering a failed first push
# uses the existing single-project path and then refreshes registration.
(cd "$failing/a-rejected" && publish --yes) >"$test_root/retry.log" 2>&1 || {
  cat "$test_root/retry.log" >&2
  fail 'direct retry after a failed first push did not recover'
}
assert_same_ref "$failing/a-rejected" a-rejected refs/heads/main
checkouts=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 checkouts' "$CK_GIT_SHELL" \
  --client-id bulk-test --repo-root "$test_root/repos" --control-socket "$test_root/control.sock")
contains "$checkouts" 'a-rejected '

mkdir "$test_root/empty"
publish --yes "$test_root/empty" >"$test_root/empty.log" 2>&1 || fail 'empty folder was not a successful no-op'
echo 'bulk publish integration tests passed'
