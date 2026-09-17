#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

set -eu
test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
[ -d "$test_root_parent" ] || { echo 'Approved temporary volume unavailable' >&2; exit 1; }
case "${TMPDIR:-}" in "$test_root_parent"/*) ;; *) echo 'Explicit approved TMPDIR required' >&2; exit 1 ;; esac
test_root=$(mktemp -d "$test_root_parent/ckr.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
mkdir "$test_root/tmp" "$test_root/repos" "$test_root/state" "$test_root/hooks" \
  "$test_root/restore-repos" "$test_root/restore-state" "$test_root/template"
chmod 0700 "$test_root/state" "$test_root/restore-state"
TMPDIR="$test_root/tmp"
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_TEMPLATE_DIR="$test_root/template"
GIT_AUTHOR_NAME='Recovery Tests'
GIT_AUTHOR_EMAIL=tests@example.test
GIT_COMMITTER_NAME='Recovery Tests'
GIT_COMMITTER_EMAIL=tests@example.test
export TMPDIR GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_TEMPLATE_DIR \
  GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL

fail() { echo "recovery integration: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) ;; *) fail "expected '$2' in: $1" ;; esac; }
capture() {
  set +e
  "$@" >"$test_root/out" 2>"$test_root/err"
  command_status=$?
  set -e
  command_output=$(cat "$test_root/out" "$test_root/err")
}
succeeded() { [ "$command_status" -eq 0 ] || fail "command failed: $command_output"; }
rejected() { [ "$command_status" -ne 0 ] || fail "unsafe recovery unexpectedly succeeded: $command_output"; }
admin() { "$CKGIT_ADMIN" "$@" --repo-root "$test_root/repos" --state-root "$test_root/state"; }
target() { "$CKGIT_ADMIN" "$@" --config "$test_root/target.ini"; }
refs() { git -C "$1" for-each-ref --format='%(refname) %(objectname)' | sort; }
printf '#!/bin/sh\nexit 0\n' >"$test_root/hooks/post-receive"
chmod 0700 "$test_root/hooks/post-receive"
printf 'schema_version=1\nrepo_root=%s\nstate_root=%s\ncontrol_socket=%s\nhook_directory=%s\n' \
  "$test_root/restore-repos" "$test_root/restore-state" "$test_root/no-daemon.sock" "$test_root/hooks" >"$test_root/target.ini"

for verb in backup verify-backup restore-backup trash restore-project; do
  capture "$CKGIT_ADMIN" "$verb" --help
  succeeded
  contains "$command_output" "$verb"
  contains "$command_output" --dry-run
done
"$CKGIT_ADMIN" create alpha --repo-root "$test_root/repos" >/dev/null
"$CKGIT_ADMIN" create empty --repo-root "$test_root/repos" --default-branch future >/dev/null
git init -q --initial-branch=main "$test_root/seed"
printf 'initial\n' >"$test_root/seed/README.md"
git -C "$test_root/seed" add README.md
git -C "$test_root/seed" -c commit.gpgSign=false commit -q -m initial
first=$(git -C "$test_root/seed" rev-parse HEAD)
git -C "$test_root/seed" tag -a release/v1 -m release
git -C "$test_root/seed" branch feature/one
git -C "$test_root/seed" update-ref refs/archive/retained "$first"
git -C "$test_root/seed" push -q --mirror "$test_root/repos/alpha.git"
git -C "$test_root/repos/alpha.git" symbolic-ref refs/heads/current refs/heads/main
original_refs=$(refs "$test_root/repos/alpha.git")

# Reachability from detached HEAD is part of the backup contract even when no
# branch or tag points at that extra commit.
"$CKGIT_ADMIN" create detached --repo-root "$test_root/repos" >/dev/null
git -C "$test_root/seed" push -q "$test_root/repos/detached.git" main
detached=$(git -C "$test_root/repos/detached.git" commit-tree "$first^{tree}" -p "$first" -m 'Detached hosted HEAD')
printf '%s\n' "$detached" >"$test_root/repos/detached.git/HEAD"

# Read-only previews validate before asking to write. Piped invocation without
# --yes remains a preview, and --dry-run always wins over --yes.
capture admin backup --output "$test_root/backup" --dry-run --yes
succeeded
contains "$command_output" 'Would back up 3 project(s)'
[ ! -e "$test_root/backup" ] || fail 'dry run created backup'
capture admin backup --output "$test_root/backup"
succeeded
contains "$command_output" 'use --yes'
[ ! -e "$test_root/backup" ] || fail 'redirected input authorized backup'
capture admin backup --output "$test_root/backup" --yes
succeeded
contains "$command_output" 'verified and saved'
capture "$CKGIT_ADMIN" verify-backup "$test_root/backup"
succeeded
contains "$command_output" '3 project(s)'
[ "$(refs "$test_root/backup/repositories/alpha.git")" = "$original_refs" ] || fail 'backup lost branches/tags/custom refs'
[ "$(git -C "$test_root/backup/repositories/alpha.git" symbolic-ref refs/heads/current)" = refs/heads/main ] || fail 'backup lost symbolic ref identity'
[ "$(git -C "$test_root/backup/repositories/detached.git" rev-parse HEAD)" = "$detached" ] || fail 'backup lost detached HEAD'
[ "$(git -C "$test_root/backup/repositories/empty.git" symbolic-ref HEAD)" = refs/heads/future ] || fail 'backup lost unborn HEAD'
cmp "$test_root/repos/alpha.git/config" "$test_root/backup/configs/alpha.config" || fail 'original config not preserved'
capture admin backup --output "$test_root/backup" --yes
rejected
contains "$command_output" 'already exists'

capture target restore-backup "$test_root/backup" --dry-run --yes
succeeded
[ ! -e "$test_root/restore-repos/alpha.git" ] || fail 'restore preview wrote repository'
capture target restore-backup "$test_root/backup" --yes
succeeded
[ "$(refs "$test_root/restore-repos/alpha.git")" = "$original_refs" ] || fail 'restore lost refs'
[ "$(git -C "$test_root/restore-repos/alpha.git" symbolic-ref refs/heads/current)" = refs/heads/main ] || fail 'restore lost symbolic ref identity'
[ "$(git -C "$test_root/restore-repos/detached.git" rev-parse HEAD)" = "$detached" ] || fail 'restore lost detached HEAD'
[ "$(git -C "$test_root/restore-repos/alpha.git" config core.hooksPath)" = "$test_root/hooks" ] || fail 'target hooks not installed'
[ "$(git -C "$test_root/restore-repos/alpha.git" config receive.denyNonFastForwards)" = true ] || fail 'receive safety not reinstated'
[ -z "$(git -C "$test_root/restore-repos/alpha.git" remote)" ] || fail 'restored repository has source mirror remote'
capture target restore-backup "$test_root/backup" --yes
rejected
[ "$(refs "$test_root/restore-repos/alpha.git")" = "$original_refs" ] || fail 'collision overwrote repository'

# Verification refuses tampering and missing objects, without reaching a
# restore destination. The source backup remains usable after each fixture.
cp -R "$test_root/backup" "$test_root/tampered"
printf 'tampered\n' >>"$test_root/tampered/configs/alpha.config"
capture "$CKGIT_ADMIN" verify-backup "$test_root/tampered"
rejected
contains "$command_output" 'manifest'
cp -R "$test_root/backup" "$test_root/missing-objects"
rm -rf "$test_root/missing-objects/repositories/alpha.git/objects"
capture "$CKGIT_ADMIN" verify-backup "$test_root/missing-objects"
rejected
ln -s "$test_root/backup" "$test_root/backup-link"
capture "$CKGIT_ADMIN" verify-backup "$test_root/backup-link"
rejected
capture admin backup --output "$test_root/repos/nested-backup" --yes
rejected
[ ! -e "$test_root/repos/nested-backup" ] || fail 'backup wrote inside source root'

# Trash remains a Git-only recovery copy. Its basename is accepted through
# '--' even when a valid project name resembles an option.
capture admin remove-project alpha
succeeded
contains "$command_output" 'Preview only; use --yes'
[ -d "$test_root/repos/alpha.git" ] || fail 'preview without --yes removed the repository'
capture admin remove-project alpha --yes
succeeded
entry=$(basename "$(find "$test_root/repos/.trash" -mindepth 1 -maxdepth 1 -type d -name 'alpha.git.*')")
capture admin trash list
succeeded
contains "$command_output" "$entry"
capture admin restore-project "$entry" --dry-run
succeeded
[ ! -e "$test_root/repos/alpha.git" ] || fail 'trash preview recreated repository'
capture admin restore-project "$entry" --yes --hook-directory "$test_root/hooks"
succeeded
contains "$command_output" 'no metadata recreated'
[ "$(refs "$test_root/repos/alpha.git")" = "$original_refs" ] || fail 'trash recovery lost refs'
[ -d "$test_root/repos/.trash/$entry" ] || fail 'trash recovery discarded retained copy'
[ ! -e "$test_root/state/checkouts/alpha" ] || fail 'trash recovery recreated checkout metadata'
capture admin restore-project "$entry" --yes
rejected

"$CKGIT_ADMIN" create --config --repo-root "$test_root/repos" >/dev/null
capture admin remove-project --config --yes
succeeded
dash_entry=$(basename "$(find "$test_root/repos/.trash" -mindepth 1 -maxdepth 1 -type d -name '--config.git.*')")
capture "$CKGIT_ADMIN" restore-project --repo-root "$test_root/repos" --state-root "$test_root/state" --yes -- "$dash_entry"
succeeded
[ -d "$test_root/repos/--config.git" ] || fail 'option-like project could not be restored'
capture "$CKGIT_ADMIN" verify-backup "$test_root/backup"
succeeded
printf 'recovery command journeys passed\n'
