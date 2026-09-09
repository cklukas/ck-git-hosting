#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

# Real hosted Git histories exercise incoming updates without touching any
# physical user project. Only the SSH transport is redirected to the fixture.
set -eu
test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
[ -d "$test_root_parent" ] || { echo 'Approved temporary volume is unavailable' >&2; exit 1; }
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/cki.XXXXXX")
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$test_root/tmp" "$test_root/repos" "$test_root/state" "$test_root/hooks" \
  "$test_root/test-bin" "$test_root/producer" "$test_root/consumer" "$test_root/working"
chmod 0700 "$test_root/state"
TMPDIR="$test_root/tmp"
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_TERMINAL_PROMPT=0
CKGIT_TEST_CLIENT_ID=incoming-consumer
TEST_ROOT="$test_root"
export TMPDIR GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_TERMINAL_PROMPT CKGIT_TEST_CLIENT_ID TEST_ROOT
unset GIT_SSH GIT_SSH_COMMAND

fail() { echo "incoming updates: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) ;; *) fail "expected '$2' in: $1" ;; esac; }
capture() {
  set +e
  "$@" >"$test_root/command.out" 2>"$test_root/command.err"
  command_status=$?
  set -e
  command_output=$(cat "$test_root/command.out" "$test_root/command.err")
}
succeeded() { [ "$command_status" -eq 0 ] || fail "command returned $command_status: $command_output"; }
refused() { [ "$command_status" -eq 3 ] || fail "expected attention exit 3, got $command_status: $command_output"; }
head() { git -C "$1" rev-parse HEAD; }
ref() { git -C "$1" rev-parse --verify "$2"; }
absent() {
  if git -C "$1" show-ref --verify --quiet "$2"; then fail "unexpected ref $2 in $1"; fi
}
local_refs() { git -C "$1" for-each-ref --format='%(refname) %(objectname)' refs/heads refs/tags; }
all_refs() { git -C "$1" for-each-ref --format='%(refname) %(objectname)'; }
git_file_hash() {
  git_path=$(git -C "$1" rev-parse --git-path "$2")
  case "$git_path" in /*) ;; *) git_path="$1/$git_path" ;; esac
  if [ -f "$git_path" ]; then git -C "$1" hash-object "$git_path"; else printf 'absent\n'; fi
}
commit() { git -C "$1" -c commit.gpgSign=false commit -q -m "$2"; }
identity() {
  git -C "$1" config user.name 'Incoming Journey Tests'
  git -C "$1" config user.email tests@example.test
}
write_config() {
  printf '%s\n' schema_version=1 "client_id=$2" "display_name=$2" server=ckgit@fixture \
    remote_name=ckgit public_path_mode=basename >"$1"
}
write_config "$test_root/producer/client.ini" incoming-producer
write_config "$test_root/consumer/client.ini" incoming-consumer
cp "$CKGIT_POST_RECEIVE" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
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
  [ "$attempt" -lt 100 ] || fail 'fixture control socket did not become ready'
  sleep .1
done
cat >"$test_root/test-bin/ssh" <<'SH'
#!/bin/sh
for argument do request=$argument; done
printf '%s\n' "$request" >>"$TEST_ROOT/ssh-calls"
SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id "$CKGIT_TEST_CLIENT_ID" \
  --repo-root "$TEST_ROOT/repos" --control-socket "$TEST_ROOT/control.sock" --state-root "$TEST_ROOT/state"
SH
chmod 0700 "$test_root/test-bin/ssh"
PATH="$test_root/test-bin:$PATH"
export PATH
client() { "$CKGIT" "$@" --config "$test_root/consumer/client.ini"; }
producer() {
  CKGIT_TEST_CLIENT_ID=incoming-producer "$CKGIT" "$@" --config "$test_root/producer/client.ini"
}
new_project() {
  git init -q --initial-branch=main "$test_root/producer/$1"
  identity "$test_root/producer/$1"
  printf 'initial %s\n' "$1" >"$test_root/producer/$1/README.md"
  git -C "$test_root/producer/$1" add README.md
  commit "$test_root/producer/$1" 'Initial published content'
  capture producer publish --yes "$test_root/producer/$1"
  succeeded
  capture client clone "$1" "$test_root/working/$1"
  succeeded
  identity "$test_root/working/$1"
}

for verb in fetch update; do
  capture "$CKGIT" "$verb" --config "$test_root/missing.ini" --help
  succeeded
  contains "$command_output" '--project'
  capture client "$verb" --repo . --all
  [ "$command_status" -eq 2 ] || fail "$verb accepts conflicting selectors"
  capture client "$verb" --project unregistered --dry-run
  refused
  contains "$command_output" 'not managed'
done

new_project alpha
new_project beta
new_project missing
alpha="$test_root/working/alpha"
alpha_source="$test_root/producer/alpha"
beta="$test_root/working/beta"
beta_source="$test_root/producer/beta"
initial=$(head "$alpha")
beta_initial=$(head "$beta")
git -C "$alpha" branch feature/local "$initial"
git -C "$alpha" tag keep-local "$initial"
git -C "$alpha" update-ref refs/remotes/ckgit/retired "$initial"
git -C "$alpha_source" tag release/one "$initial"
printf 'second alpha\n' >"$alpha_source/README.md"
git -C "$alpha_source" add README.md
commit "$alpha_source" 'Published update from another device'
second=$(head "$alpha_source")
git -C "$alpha_source" branch feature/new
git -C "$alpha_source" tag -a release/two -m 'Annotated release'
capture producer sync --repo "$alpha_source"
succeeded

# A preview advertises actual hosted refs without writing FETCH_HEAD, refs,
# configuration, the index, or private checkout selections.
before_refs=$(all_refs "$alpha")
before_fetch=$(git_file_hash "$alpha" FETCH_HEAD)
before_index=$(git_file_hash "$alpha" index)
before_config=$(git_file_hash "$alpha" config)
before_management=$(cat "$test_root/consumer/canonical.ini")
capture client update --project alpha --dry-run --verbose
succeeded
contains "$command_output" 'Would fetch'
contains "$command_output" refs/heads/main
contains "$command_output" 'Would update only refs/heads/main'
contains "$command_output" 'Ancestry will be checked after fetch'
[ "$(all_refs "$alpha")" = "$before_refs" ] || fail 'update preview changed refs'
[ "$(git_file_hash "$alpha" FETCH_HEAD)" = "$before_fetch" ] || fail 'preview wrote FETCH_HEAD'
[ "$(git_file_hash "$alpha" index)" = "$before_index" ] || fail 'preview wrote index'
[ "$(git_file_hash "$alpha" config)" = "$before_config" ] || fail 'preview wrote configuration'
[ "$(cat "$test_root/consumer/canonical.ini")" = "$before_management" ] || fail 'preview changed management'
capture client fetch --repo "$alpha" --branch feature/new --no-tags --dry-run
succeeded
contains "$command_output" 'refs/heads/feature/new'
contains "$command_output" 'refs/remotes/ckgit/feature/new'
[ "$(all_refs "$alpha")" = "$before_refs" ] || fail 'fetch preview changed refs'

# Dangerous user-configured fetch destinations/pruning do not broaden this
# operation. Even fetching main must never write refs/heads/main.
git -C "$alpha" config --replace-all remote.ckgit.fetch '+refs/heads/*:refs/heads/*'
git -C "$alpha" config remote.ckgit.prune true
git -C "$alpha" config remote.ckgit.pruneTags true
git -C "$alpha" config fetch.prune true
git -C "$alpha" config fetch.pruneTags true
before_local=$(local_refs "$alpha")
capture client fetch --repo "$alpha" --branch feature/new --no-tags
succeeded
[ "$(ref "$alpha" refs/remotes/ckgit/feature/new)" = "$second" ] || fail 'selected branch was not fetched'
[ "$(ref "$alpha" refs/remotes/ckgit/main)" = "$initial" ] || fail 'branch-limited fetch changed main tracking ref'
[ "$(local_refs "$alpha")" = "$before_local" ] || fail 'branch-limited fetch changed local refs'
[ "$(cat "$alpha/README.md")" = 'initial alpha' ] || fail 'fetch changed working files'
absent "$alpha" refs/tags/release/two

# A destination must be a direct ref. Otherwise Git may follow a symbolic
# remote-tracking/tag ref and overwrite the local branch it points to.
git -C "$alpha" symbolic-ref refs/remotes/ckgit/main refs/heads/feature/local
capture client fetch --repo "$alpha" --branch main --no-tags
refused
contains "$command_output" 'is a symbolic reference'
[ "$(ref "$alpha" refs/heads/feature/local)" = "$initial" ] || fail 'fetch followed a symbolic tracking ref'
git -C "$alpha" symbolic-ref --delete refs/remotes/ckgit/main
git -C "$alpha" symbolic-ref refs/remotes/ckgit/main refs/heads/not-created
capture client fetch --repo "$alpha" --branch main --no-tags
refused
contains "$command_output" 'is a symbolic reference'
absent "$alpha" refs/heads/not-created
git -C "$alpha" symbolic-ref --delete refs/remotes/ckgit/main
git -C "$alpha" update-ref refs/remotes/ckgit/main "$initial"
git -C "$alpha" symbolic-ref refs/tags/release/one refs/heads/feature/local
capture client fetch --repo "$alpha"
refused
contains "$command_output" 'is a symbolic reference'
[ "$(ref "$alpha" refs/heads/feature/local)" = "$initial" ] || fail 'fetch followed a symbolic tag'
git -C "$alpha" symbolic-ref --delete refs/tags/release/one
capture client fetch --project alpha --verbose
succeeded
[ "$(head "$alpha")" = "$initial" ] || fail 'fetch advanced current branch'
[ "$(ref "$alpha" refs/heads/feature/local)" = "$initial" ] || fail 'fetch changed another local branch'
[ "$(ref "$alpha" refs/remotes/ckgit/main)" = "$second" ] || fail 'fetch did not update remote tracking branch'
[ "$(ref "$alpha" refs/tags/release/two)" = "$(ref "$alpha_source" refs/tags/release/two)" ] || fail 'annotated tag not preserved'
[ "$(ref "$alpha" refs/tags/keep-local)" = "$initial" ] || fail 'fetch pruned local tag'
[ "$(ref "$alpha" refs/remotes/ckgit/retired)" = "$initial" ] || fail 'fetch pruned tracking ref'
capture client update --project alpha
succeeded
contains "$command_output" 'by fast-forward'
[ "$(head "$alpha")" = "$second" ] || fail 'update did not fast-forward current branch'
[ "$(cat "$alpha/README.md")" = 'second alpha' ] || fail 'update did not update working files'
[ "$(ref "$alpha" refs/heads/feature/local)" = "$initial" ] || fail 'update advanced another local branch'
[ "$(head "$beta")" = "$beta_initial" ] || fail '--project update touched another project'
capture client update --repo "$alpha"
succeeded
contains "$command_output" 'Already up to date'

# Dirty tracked files, staged changes, and untracked collisions are refused
# before fetch. The user's exact files and index are retained in every case.
printf 'third alpha\n' >"$alpha_source/README.md"
printf 'incoming content\n' >"$alpha_source/new-file.txt"
git -C "$alpha_source" add README.md new-file.txt
commit "$alpha_source" 'More published work and a new file'
third=$(head "$alpha_source")
capture producer sync --repo "$alpha_source"
succeeded
printf 'local unsaved work\n' >"$alpha/README.md"
capture client update --repo "$alpha"
refused
contains "$command_output" 'working tree is not clean'
[ "$(cat "$alpha/README.md")" = 'local unsaved work' ] || fail 'dirty tracked work was lost'
[ "$(head "$alpha")" = "$second" ] || fail 'dirty update moved HEAD'
[ "$(ref "$alpha" refs/remotes/ckgit/main)" = "$second" ] || fail 'dirty refusal performed fetch'
git -C "$alpha" add README.md
staged_index=$(git_file_hash "$alpha" index)
capture client update --repo "$alpha"
refused
[ "$(git_file_hash "$alpha" index)" = "$staged_index" ] || fail 'staged index was modified'
[ "$(git -C "$alpha" show :README.md)" = 'local unsaved work' ] || fail 'staged content was lost'
git -C "$alpha" restore --source=HEAD --staged --worktree README.md
printf 'untracked work to keep\n' >"$alpha/new-file.txt"
capture client update --repo "$alpha"
refused
[ "$(cat "$alpha/new-file.txt")" = 'untracked work to keep' ] || fail 'untracked content was lost'
[ "$(head "$alpha")" = "$second" ] || fail 'untracked refusal moved HEAD'
rm "$alpha/new-file.txt"

# Detached and unpublished local branches need an explicit user choice.
git -C "$alpha" checkout -q --detach "$second"
capture client update --repo "$alpha"
refused
contains "$command_output" 'detached HEAD'
[ "$(head "$alpha")" = "$second" ] || fail 'detached HEAD was moved'
git -C "$alpha" checkout -q -b local-only
capture client update --repo "$alpha"
refused
contains "$command_output" 'hosted branch local-only is missing'
[ "$(head "$alpha")" = "$second" ] || fail 'missing branch refusal moved HEAD'
git -C "$alpha" checkout -q main
capture client fetch --repo "$alpha" --branch not-published --no-tags
refused
contains "$command_output" 'hosted branch not-published is missing'
[ "$(ref "$alpha" refs/remotes/ckgit/main)" = "$second" ] || fail 'missing branch request changed refs'

# A real divergent graph must remain divergent: neither a merge commit nor
# rebase/reset is an acceptable implicit resolution.
printf 'local committed work\n' >"$alpha/local-only.txt"
git -C "$alpha" add local-only.txt
commit "$alpha" 'Local commit that is not on the hosted branch'
divergent=$(head "$alpha")
local_before_divergence=$(local_refs "$alpha")
capture client update --repo "$alpha"
refused
contains "$command_output" 'have diverged'
[ "$(head "$alpha")" = "$divergent" ] || fail 'divergent local HEAD was changed'
[ "$(local_refs "$alpha")" = "$local_before_divergence" ] || fail 'divergence changed local refs'
[ "$(ref "$alpha" refs/remotes/ckgit/main)" = "$third" ] || fail 'divergence did not leave downloaded history available'
[ "$(cat "$alpha/local-only.txt")" = 'local committed work' ] || fail 'divergent local file was lost'
[ "$(git -C "$alpha" rev-list --count refs/remotes/ckgit/main..HEAD)" -eq 1 ] || fail 'local divergent history was altered'
[ ! -f "$alpha/.git/MERGE_HEAD" ] || fail 'update started a merge'
capture client update --repo "$alpha" --dry-run
refused
contains "$command_output" 'have diverged'
[ "$(head "$alpha")" = "$divergent" ] || fail 'divergent preview moved HEAD'

# Replacement objects and legacy grafts must not disguise that divergence as
# an apparent fast-forward and discard the stored local ancestry.
replacement=$(git -C "$alpha" commit-tree "$third^{tree}" -p "$divergent" -m 'Synthetic replacement history')
git -C "$alpha" replace "$third" "$replacement"
capture client update --repo "$alpha"
refused
contains "$command_output" 'have diverged'
[ "$(head "$alpha")" = "$divergent" ] || fail 'replacement history moved local HEAD'
git -C "$alpha" replace -d "$third" >/dev/null
printf '%s %s\n' "$third" "$divergent" >"$alpha/.git/info/grafts"
capture client update --repo "$alpha"
refused
contains "$command_output" 'legacy Git grafts'
[ "$(head "$alpha")" = "$divergent" ] || fail 'grafted history moved local HEAD'
rm "$alpha/.git/info/grafts"

# An already-ahead branch remains usable and is never moved backwards.
git -C "$alpha_source" branch ahead "$third"
capture producer sync --repo "$alpha_source"
succeeded
git -C "$alpha" checkout -q -b ahead "$third"
printf 'ahead work\n' >"$alpha/ahead.txt"
git -C "$alpha" add ahead.txt
commit "$alpha" 'Local branch ahead of its hosted counterpart'
ahead=$(head "$alpha")
capture client update --repo "$alpha"
succeeded
contains "$command_output" 'Local branch is ahead'
[ "$(head "$alpha")" = "$ahead" ] || fail 'ahead branch was moved backwards'
git -C "$alpha" checkout -q main

# Existing tag names are immutable. A conflict rejects the whole atomic fetch
# and a subsequent branch-only fetch remains available as the recovery path.
git -C "$alpha_source" tag conflicting "$third"
capture producer sync --repo "$alpha_source"
succeeded
git -C "$alpha" tag conflicting "$initial"
before_conflict=$(all_refs "$alpha")
capture client fetch --repo "$alpha"
refused
contains "$command_output" 'tag conflicting differs'
[ "$(all_refs "$alpha")" = "$before_conflict" ] || fail 'tag conflict modified refs'
capture client fetch --repo "$alpha" --no-tags
succeeded
[ "$(ref "$alpha" refs/tags/conflicting)" = "$initial" ] || fail 'branch-only fetch overwrote tag'
git -C "$alpha" tag -d conflicting >/dev/null

# The managed sweep continues after a conflicting project and reports missing
# paths while advancing an independent clean project. It never scans folders.
printf 'second beta\n' >"$beta_source/README.md"
printf 'published ignored-path content\n' >"$beta_source/ignored.txt"
git -C "$beta_source" add README.md ignored.txt
commit "$beta_source" 'Independent project update'
beta_second=$(head "$beta_source")
capture producer sync --repo "$beta_source"
succeeded
# Git normally overwrites ignored files on a fast-forward. Protect that local
# data too, although ordinary porcelain status deliberately omits it.
printf 'ignored.txt\n' >>"$beta/.git/info/exclude"
printf 'ignored local work\n' >"$beta/ignored.txt"
capture client update --repo "$beta"
refused
[ "$(head "$beta")" = "$beta_initial" ] || fail 'ignored-file collision moved HEAD'
[ "$(cat "$beta/ignored.txt")" = 'ignored local work' ] || fail 'ignored local data was overwritten'
[ "$(cat "$beta/README.md")" = 'initial beta' ] || fail 'failed update partially changed tracked files'
rm "$beta/ignored.txt"
# User merge defaults must not silently turn an update into a squash, leaving
# changes staged without advancing the actual current branch.
git -C "$beta" config -- branch.main.mergeOptions --squash
mv "$test_root/working/missing" "$test_root/working/missing-away"
git init -q --initial-branch=main "$test_root/working/not-managed"
identity "$test_root/working/not-managed"
git -C "$test_root/working/not-managed" commit -q --allow-empty -m 'Unmanaged local project'
capture client update
refused
contains "$command_output" '3 managed project(s)'
contains "$command_output" 'have diverged'
contains "$command_output" 'missing'
contains "$command_output" 'unavailable'
[ "$(head "$beta")" = "$beta_second" ] || fail 'managed sweep did not continue to clean project'
[ "$(head "$alpha")" = "$divergent" ] || fail 'managed sweep modified conflicting project'
[ -z "$(git -C "$test_root/working/not-managed" remote)" ] || fail 'managed sweep adopted an unrelated folder'
capture client fetch --all
refused
contains "$command_output" 'missing'
contains "$command_output" 'Fetched beta'
[ "$(head "$alpha")" = "$divergent" ] || fail 'managed fetch changed local branch'

# Pairing is checked for every selected checkout; a stale private mapping must
# not turn a request for alpha into a download from another hosted project.
git -C "$beta" remote set-url ckgit ckgit@fixture:alpha.git
beta_before=$(all_refs "$beta")
capture client update --project beta
refused
contains "$command_output" 'no longer paired'
[ "$(all_refs "$beta")" = "$beta_before" ] || fail 'changed pairing modified refs'
git -C "$beta" remote set-url ckgit ckgit@fixture:beta.git

printf 'incoming update journeys passed\n'
