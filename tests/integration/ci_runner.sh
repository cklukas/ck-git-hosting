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
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
fail() { echo "$1" >&2; exit 1; }

: "${CKGIT_ADMIN:?CKGIT_ADMIN must point at ckgit-admin}"
: "${CK_CI_RUNNER:?CK_CI_RUNNER must point at ck-ci-runnerd}"
: "${CKGIT_POST_RECEIVE:?CKGIT_POST_RECEIVE must point at the post-receive hook}"
: "${CKGIT_HOSTINGD:?CKGIT_HOSTINGD must point at ck-git-hostingd}"

repos="$test_root/repos"
state="$test_root/state"
build="$test_root/build"
work="$test_root/work"
mkdir -p "$repos" "$build"
mkdir -p "$state"; chmod 700 "$state"

# A commit that carries a workflow, pushed into the bare hosted repository.
git -c init.defaultBranch=main init -q "$work"
mkdir -p "$work/.ckgit"
cat > "$work/.ckgit/ci.yml" <<'YML'
version: 1
jobs:
  - name: build
    steps:
      - run: sh -ec 'echo integ-ci-ok; mkdir -p out; printf payload > out/artifact.txt'
    artifacts:
      name: build
      paths: [out]
YML
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

run_id=$(basename "$(dirname "$run_ini")")

# The job's declared artifact was packed into the run's artifact store.
run_dir=$(dirname "$run_ini")
[ -f "$run_dir/artifacts/build.tar" ] || fail "the artifact bundle was not stored"
tar -tf "$run_dir/artifacts/build.tar" | grep -q 'out/artifact.txt' || fail "the artifact bundle lacks the built file"

# The read-only dashboard shows the run and serves its step log. The daemon
# started after the run already has it indexed on start-up.
"$CKGIT_HOSTINGD" --repo-root "$repos" --state-root "$state" \
  --control-socket "$test_root/control.sock" --http-port 0 \
  > "$test_root/server.log" 2>&1 &
server_pid=$!
attempt=0; port=''
while [ -z "$port" ]; do
  kill -0 "$server_pid" 2>/dev/null || { cat "$test_root/server.log" >&2; fail "daemon exited"; }
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$test_root/server.log")
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail "daemon listener never became ready"
  [ -n "$port" ] || sleep .1
done
base="http://127.0.0.1:$port"

attempt=0
while :; do
  curl --path-as-is --max-time 4 --silent -o "$test_root/ci.html" "$base/project/demo/ci" || fail "curl ci page"
  grep -q "success" "$test_root/ci.html" && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || { cat "$test_root/ci.html"; fail "CI page never showed the run"; }
  sleep .1
done
grep -q "/project/demo/ci/$run_id/0.log" "$test_root/ci.html" || fail "CI page has no step log link"

curl --path-as-is --max-time 4 --silent -o "$test_root/ci-log.html" "$base/project/demo/ci/$run_id/0.log" \
  || fail "curl ci log"
grep -q "integ-ci-ok" "$test_root/ci-log.html" || { cat "$test_root/ci-log.html"; fail "log view missing step output"; }

# The dashboard links the artifact and serves the bundle as a downloadable tar.
grep -q "/project/demo/ci/$run_id/artifacts/build" "$test_root/ci.html" || fail "CI page has no artifact link"
curl --path-as-is --max-time 4 --silent -o "$test_root/build.tar" \
  "$base/project/demo/ci/$run_id/artifacts/build" || fail "curl artifact"
tar -tf "$test_root/build.tar" | grep -q 'out/artifact.txt' || fail "downloaded artifact is not the expected bundle"

# A tag push turns the same build's artifacts into a durable release.
git -C "$work" -c user.email=t@example.invalid -c user.name=Test tag -a -m 'Release 1.0' v1.0.0
git -C "$work" push -q "$repos/demo.git" v1.0.0
tag_id=$(git -C "$work" rev-parse v1.0.0)
printf '%s %s refs/tags/v1.0.0\n' "$(printf '0%.0s' $(seq 1 40))" "$tag_id" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive failed for the tag"
"$CK_CI_RUNNER" serve --config "$test_root/server.ini" --once >/dev/null 2>&1 || fail "serve (tag) failed"
[ -f "$state/releases/demo/v1.0.0/release.ini" ] || fail "the release record was not written"
[ -f "$state/releases/demo/v1.0.0/build.tar" ] || fail "the release asset was not stored"
grep -q "status=success" "$(ls "$state"/ci/runs/demo/*/run.ini | sort | tail -n1)" || fail "the tag build did not succeed"

attempt=0
while :; do
  curl --path-as-is --max-time 4 --silent -o "$test_root/rel.html" "$base/project/demo/releases" || fail "curl releases"
  grep -q "v1.0.0" "$test_root/rel.html" && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || { cat "$test_root/rel.html"; fail "releases page never showed the tag"; }
  sleep .1
done
grep -q "/project/demo/releases/v1.0.0/build" "$test_root/rel.html" || fail "releases page has no asset link"
curl --path-as-is --max-time 4 --silent -o "$test_root/rel.tar" \
  "$base/project/demo/releases/v1.0.0/build" || fail "curl release asset"
tar -tf "$test_root/rel.tar" | grep -q 'out/artifact.txt' || fail "release asset is not the expected bundle"

# Deleting the tag drops its release and every asset.
printf '%s %s refs/tags/v1.0.0\n' "$tag_id" "$(printf '0%.0s' $(seq 1 40))" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive failed for the tag deletion"
[ ! -e "$state/releases/demo/v1.0.0" ] || fail "the release survived its tag being deleted"

echo "ci_runner integration OK"
