#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end CI: a CI-enabled push runs the post-receive hook, which queues a
# job; ck-ci-runnerd drains the spool, runs the workflow from the commit, and
# records the result. No server or network is involved.

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckci.XXXXXX")
cleanup() { rm -rf "$test_root"; }
trap cleanup EXIT HUP INT TERM
fail() { echo "$1" >&2; exit 1; }

: "${CKGIT_ADMIN:?CKGIT_ADMIN must point at ckgit-admin}"
: "${CK_CI_RUNNER:?CK_CI_RUNNER must point at ck-ci-runnerd}"
: "${CKGIT_POST_RECEIVE:?CKGIT_POST_RECEIVE must point at the post-receive hook}"

repos="$test_root/repos"
state="$test_root/state"
build="$test_root/build"
work="$test_root/work"
mkdir -p "$repos" "$build"
mkdir -p "$state"; chmod 700 "$state"

# A commit that carries a workflow, pushed into the bare hosted repository.
git -c init.defaultBranch=main init -q "$work"
mkdir -p "$work/.ckgit"
printf 'version: 1\njobs:\n  - name: build\n    steps:\n      - run: echo integ-ci-ok\n' \
  > "$work/.ckgit/ci.yml"
git -C "$work" add -A
git -C "$work" -c user.email=t@example.invalid -c user.name=Test commit -q -m workflow
commit=$(git -C "$work" rev-parse HEAD)
git -C "$repos" -c init.defaultBranch=main init --bare -q demo.git
git -C "$work" push -q "$repos/demo.git" main

cat > "$test_root/server.ini" <<INI
schema_version=1
repo_root=$repos
control_socket=$test_root/control.sock
state_root=$state
ci_build_root=$build
INI

# Opt the project in, and confirm the status reads back.
"$CKGIT_ADMIN" ci enable demo --config "$test_root/server.ini" >/dev/null || fail "ci enable failed"
"$CKGIT_ADMIN" ci status demo --state-root "$state" | grep -q "CI enabled" || fail "ci status not enabled"

# A push to a project that has NOT opted in must queue nothing.
git -C "$repos" -c init.defaultBranch=main init --bare -q other.git
printf '%s %s refs/heads/main\n' "$(printf '0%.0s' $(seq 1 40))" "$commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=other \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "hook failed for opted-out project"
[ -z "$(ls -A "$state/ci/spool" 2>/dev/null || true)" ] || fail "an opted-out push queued a job"

# The CI-enabled push queues exactly one job.
printf '%s %s refs/heads/main\n' "$(printf '0%.0s' $(seq 1 40))" "$commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive hook failed"
ls "$state"/ci/spool/*.ini >/dev/null 2>&1 || fail "the CI-enabled push queued no job"

# The runner drains the spool and records a successful run.
"$CK_CI_RUNNER" serve --config "$test_root/server.ini" --once >/dev/null 2>&1 || fail "serve failed"
run_ini=$(ls "$state"/ci/runs/demo/*/run.ini 2>/dev/null | head -n1 || true)
[ -n "$run_ini" ] || fail "no run record was produced"
grep -q "status=success" "$run_ini" || { cat "$run_ini"; fail "the run did not succeed"; }
grep -rq "integ-ci-ok" "$state"/ci/runs/demo/*/steps/ || fail "the step output was not captured"

# The spool is drained (the claimed job moved to working, none left pending).
[ -z "$(ls -A "$state/ci/spool" 2>/dev/null || true)" ] || fail "the spool was not drained"

echo "ci_runner integration OK"
