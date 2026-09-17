#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end CI: a CI-enabled push runs the post-receive hook, which queues a
# job; ck-ci-runnerd drains the spool, runs the workflow from the commit, and
# records the result. No server or network is involved.

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckci.XXXXXX")
server_pid=''
runner_pid=''
cleanup() {
  for pid in "$runner_pid" "$server_pid"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
fail() { echo "$1" >&2; exit 1; }

: "${CKGIT:?CKGIT must point at ckgit}"
: "${CK_GIT_SHELL:?CK_GIT_SHELL must point at ck-git-shell}"
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
  # Wait until the run is indexed (its step-log link is present) rather than for
  # the bare word "success", which now also appears in the page's inlined CI
  # status styles and would match before the async index has loaded the run.
  grep -q "/project/demo/ci/$run_id/0.log" "$test_root/ci.html" && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || { cat "$test_root/ci.html"; fail "CI page never showed the run"; }
  sleep .1
done
grep -q "ci-status ci-success" "$test_root/ci.html" || { cat "$test_root/ci.html"; fail "CI page did not show the run as successful"; }

curl --path-as-is --max-time 4 --silent -o "$test_root/ci-log.html" "$base/project/demo/ci/$run_id/0.log" \
  || fail "curl ci log"
grep -q "integ-ci-ok" "$test_root/ci-log.html" || { cat "$test_root/ci-log.html"; fail "log view missing step output"; }

# The one mutating endpoint (cancel): POST-only, and it refuses a foreign
# Origin while allowing a same-origin or opaque one. A finished run is a fine
# target — the endpoint answers on method and origin before the run matters.
code=$(curl --path-as-is --max-time 4 --silent -o /dev/null -w '%{http_code}' \
  "$base/project/demo/ci/$run_id/cancel")
[ "$code" = 405 ] || fail "a GET to the cancel endpoint should be 405 (got $code)"
code=$(curl --path-as-is --max-time 4 --silent -o /dev/null -w '%{http_code}' \
  -X POST -H 'Origin: http://evil.example' "$base/project/demo/ci/$run_id/cancel")
[ "$code" = 403 ] || fail "a cross-origin cancel POST should be refused (got $code)"
code=$(curl --path-as-is --max-time 4 --silent -o /dev/null -w '%{http_code}' \
  -X POST "$base/project/demo/ci/$run_id/cancel")
[ "$code" = 303 ] || fail "a same-origin cancel POST should redirect (got $code)"

# The step log also tails live over SSE. A finished run streams its whole log
# then closes, so a bounded curl sees the event-stream content type, the output
# as data lines, and the terminal done event.
curl --path-as-is --max-time 6 --silent -D "$test_root/stream.headers" -o "$test_root/stream.body" \
  "$base/project/demo/ci/$run_id/0.stream" || fail "curl ci log stream"
grep -qi "^Content-Type: text/event-stream" "$test_root/stream.headers" || fail "the stream is not text/event-stream"
grep -q "^data: integ-ci-ok" "$test_root/stream.body" || { cat "$test_root/stream.body"; fail "the stream did not carry the step output"; }
grep -q "^id: [0-9]" "$test_root/stream.body" || fail "the stream did not emit resumable event ids"
grep -q "^event: done" "$test_root/stream.body" || fail "the stream did not end with a done event"
# The log page carries the follow control and a nonce-scoped script policy.
curl --path-as-is --max-time 4 --silent -D "$test_root/log.headers" -o "$test_root/log.html" \
  "$base/project/demo/ci/$run_id/0.log" || fail "curl ci log page"
grep -q "id=\"ci-follow-btn\"" "$test_root/log.html" || fail "the log page has no follow control"
grep -qi "^Content-Security-Policy:.*script-src 'nonce-" "$test_root/log.headers" || fail "the log page did not scope a script nonce"

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

# WP7: `ckgit release list|download`. list goes over the SSH control RPC; a
# stub ssh hands the forced command straight to ck-git-shell against the same
# live control socket the curl checks above used, exactly as a real sshd
# would for this key. download then fetches the asset directly from the
# already-running dashboard with --dashboard-url, bypassing the interactive
# SSH tunnel `ckgit web` would otherwise open.
mkdir -p "$test_root/ssh-bin"
printf '%s\n' \
  '#!/bin/sh' \
  'for argument do request=$argument; done' \
  'SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id mac-studio --repo-root "$REPOS" --control-socket "$CONTROL_SOCKET" --state-root "$STATE"' \
  >"$test_root/ssh-bin/ssh"
chmod 0700 "$test_root/ssh-bin/ssh"
printf '%s\n' \
  'schema_version=1' \
  'client_id=mac-studio' \
  'display_name=Mac Studio' \
  'server=ckgit@rpi4' \
  'remote_name=ckgit' \
  'public_path_mode=basename' >"$test_root/client.ini"

list_output=$(PATH="$test_root/ssh-bin:$PATH" CK_GIT_SHELL="$CK_GIT_SHELL" REPOS="$repos" STATE="$state" \
  CONTROL_SOCKET="$test_root/control.sock" "$CKGIT" release list demo --config "$test_root/client.ini") \
  || fail "ckgit release list failed"
case "$list_output" in
  *v1.0.0*build*) ;;
  *) echo "$list_output" >&2; fail "release list did not show the tagged release and its asset" ;;
esac

json_output=$(PATH="$test_root/ssh-bin:$PATH" CK_GIT_SHELL="$CK_GIT_SHELL" REPOS="$repos" STATE="$state" \
  CONTROL_SOCKET="$test_root/control.sock" "$CKGIT" release list demo --config "$test_root/client.ini" --json) \
  || fail "ckgit release list --json failed"
case "$json_output" in
  *'"schema_version":1'*'"tag":"v1.0.0"'*'"name":"build"'*) ;;
  *) echo "$json_output" >&2; fail "release list --json did not report the fixture release" ;;
esac

mkdir -p "$test_root/dl"
download_output=$(PATH="$test_root/ssh-bin:$PATH" CK_GIT_SHELL="$CK_GIT_SHELL" REPOS="$repos" STATE="$state" \
  CONTROL_SOCKET="$test_root/control.sock" "$CKGIT" release download demo --config "$test_root/client.ini" \
  --dashboard-url "$base" --into "$test_root/dl") || fail "ckgit release download failed"
case "$download_output" in
  *verified*) ;;
  *) echo "$download_output" >&2; fail "release download did not report a verified checksum" ;;
esac
[ -f "$test_root/dl/build.tar" ] || fail "release download did not write build.tar"
downloaded_sha=$(sha256sum "$test_root/dl/build.tar" 2>/dev/null | cut -d' ' -f1)
[ -n "$downloaded_sha" ] || downloaded_sha=$(shasum -a 256 "$test_root/dl/build.tar" | cut -d' ' -f1)
listed_sha=$(printf '%s\n' "$json_output" | sed -n 's/.*"sha256":"\([0-9a-f]*\)".*/\1/p')
[ -n "$listed_sha" ] || fail "could not extract the listed sha256 from the JSON report"
[ "$downloaded_sha" = "$listed_sha" ] || fail "downloaded asset sha256 does not match the listing"
tar -tf "$test_root/dl/build.tar" | grep -q 'out/artifact.txt' || fail "downloaded release asset is not the expected bundle"

# Deleting the tag drops its release and every asset.
printf '%s %s refs/tags/v1.0.0\n' "$tag_id" "$(printf '0%.0s' $(seq 1 40))" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive failed for the tag deletion"
[ ! -e "$state/releases/demo/v1.0.0" ] || fail "the release survived its tag being deleted"

# --- Live cancellation -----------------------------------------------------
# A long-running build is stopped on request: ckgit-admin drops the cancel
# marker, the runner kills the step's process group and records the run as
# cancelled, well before the step's own sleep would finish.
cancel_work="$test_root/cancel-work"
git -c init.defaultBranch=main init -q "$cancel_work"
mkdir -p "$cancel_work/.ckgit"
cat > "$cancel_work/.ckgit/ci.yml" <<'YML'
version: 1
jobs:
  - name: build
    steps:
      - run: sh -ec 'echo integ-cancel-start; sleep 45; echo integ-cancel-END'
YML
git -C "$cancel_work" add -A
git -C "$cancel_work" -c user.email=t@example.invalid -c user.name=Test commit -q -m slow
slow_commit=$(git -C "$cancel_work" rev-parse HEAD)
git -C "$repos" -c init.defaultBranch=main init --bare -q slow.git
git -C "$cancel_work" push -q "$repos/slow.git" main
"$CKGIT_ADMIN" ci enable slow --config "$test_root/server.ini" >/dev/null || fail "ci enable (slow) failed"
printf '%s %s refs/heads/main\n' "$(printf '0%.0s' $(seq 1 40))" "$slow_commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=slow \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive (slow) failed"

# Run the spool in the background (not --once) so the build stays in progress.
"$CK_CI_RUNNER" serve --config "$test_root/server.ini" > "$test_root/runner.log" 2>&1 &
runner_pid=$!

attempt=0; slow_ini=''
while :; do
  slow_ini=$(ls "$state"/ci/runs/slow/*/run.ini 2>/dev/null | head -n1 || true)
  { [ -n "$slow_ini" ] && grep -q "status=running" "$slow_ini"; } && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 200 ] || { cat "$test_root/runner.log" >&2; fail "the slow run never started"; }
  sleep .1
done
slow_run=$(basename "$(dirname "$slow_ini")")
grep -q "^heartbeat_epoch=" "$slow_ini" || fail "the running record carries no heartbeat"

# Cancelling an unknown run is refused; the CLI lists the in-progress run.
"$CKGIT_ADMIN" ci cancel slow 00000000000000000000-deadbeef --state-root "$state" 2>/dev/null \
  && fail "cancelling an unknown run should fail" || true
"$CKGIT_ADMIN" ci runs slow --state-root "$state" | grep -q "$slow_run" || fail "ci runs did not list the run"

# Request cancellation via the CLI; the runner stops the build promptly.
"$CKGIT_ADMIN" ci cancel slow "$slow_run" --state-root "$state" >/dev/null || fail "ci cancel failed"
attempt=0
while :; do
  grep -q "status=cancelled" "$slow_ini" && break
  attempt=$((attempt + 1)); [ "$attempt" -lt 200 ] || { cat "$slow_ini"; fail "the run was not cancelled in time"; }
  sleep .1
done
grep -rq "integ-cancel-END" "$state"/ci/runs/slow/*/steps/ && fail "the cancelled step ran to completion" || true

kill -TERM "$runner_pid" 2>/dev/null || true
wait "$runner_pid" 2>/dev/null || true
runner_pid=''

echo "ci_runner integration OK"
