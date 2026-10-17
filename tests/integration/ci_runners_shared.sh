#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Several runners sharing one main host's queue (docs/planning/14 R8): the
# primary (`ck-ci-runnerd serve` on the main host) and a secondary (a remote
# runner reaching the main host through a stand-in for ssh). It checks that
#   - an idle secondary claims a fresh job at once, well under the old 5 s poll;
#   - the primary takes a job only when no secondary does, after its delay;
#   - a secondary killed mid-run loses its lease and the job is requeued and
#     completed by the primary as attempt 2;
#   - a cancel reaches a remote run;
#   - a tag build on the secondary publishes a complete release;
#   - an upload interrupted by a dropped connection resumes;
#   - a stream cut mid-step is resumed without duplicate log lines;
#   - checkout.depth and a sister pinned to a commit with depth work remotely;
#   - the dashboard's live log stream follows a remote run (it reads only the
#     main host's files; nothing ever connects to the runner);
#   - the secondary's build root is empty after every job;
#   - a primary stopped mid-run (a restart or package upgrade) hands its job
#     back instead of failing it, and the job completes as attempt 2.

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckrs.XXXXXX")
server_pid=''
primary_pid=''
secondary_pid=''
stop_pid() {
  if [ -n "$1" ]; then
    kill -TERM "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
  fi
}
cleanup() {
  stop_pid "$secondary_pid"
  stop_pid "$primary_pid"
  stop_pid "$server_pid"
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
fail() {
  echo "$1" >&2
  for log in primary secondary server; do
    [ -f "$test_root/$log.log" ] && sed "s/^/$log: /" "$test_root/$log.log" | tail -40 >&2
  done
  exit 1
}

: "${CK_GIT_SHELL:?CK_GIT_SHELL must point at ck-git-shell}"
: "${CKGIT_ADMIN:?CKGIT_ADMIN must point at ckgit-admin}"
: "${CK_CI_RUNNER:?CK_CI_RUNNER must point at ck-ci-runnerd}"
: "${CKGIT_POST_RECEIVE:?CKGIT_POST_RECEIVE must point at the post-receive hook}"
: "${CKGIT_HOSTINGD:?CKGIT_HOSTINGD must point at ck-git-hostingd}"

repos="$test_root/repos"
state="$test_root/state"
secondary_build="$test_root/secondary/build"
mkdir -p "$repos" "$test_root/primary-build" "$test_root/secondary" "$test_root/pages" "$test_root/marks"
mkdir -p "$state"; chmod 700 "$state"
commit_as() { git -C "$1" -c user.email=t@example.invalid -c user.name=Test commit -q "$@"; }

# A sister project with history, pinned below to its second-newest commit.
lib="$test_root/lib"
git -c init.defaultBranch=main init -q "$lib"
for n in 1 2 3 4; do echo "lib $n" > "$lib/version.txt"; git -C "$lib" add -A; commit_as "$lib" -m "lib $n"; done
lib_pin=$(git -C "$lib" rev-parse HEAD~1)
git -C "$repos" -c init.defaultBranch=main init --bare -q lib.git
git -C "$lib" push -q "$repos/lib.git" main

work="$test_root/work"
git -c init.defaultBranch=main init -q "$work"
for n in 1 2 3 4 5 6; do echo "$n" > "$work/history.txt"; git -C "$work" add -A; commit_as "$work" -m "history $n"; done
git -C "$repos" -c init.defaultBranch=main init --bare -q demo.git

# Commits $1 as the workflow and pushes it; prints the commit id.
published=0
publish() {
  published=$((published + 1))
  mkdir -p "$work/.ckgit"
  printf '%s\n' "$1" > "$work/.ckgit/ci.yml"
  git -C "$work" add -A
  commit_as "$work" -m "workflow $published" --allow-empty
  git -C "$work" push -q "$repos/demo.git" HEAD:main
  git -C "$work" rev-parse HEAD
}
# Queues a job for commit $1 on ref $2 the way a push does; prints its run id.
queue() {
  before=$(ls "$state/ci/runs/demo" 2>/dev/null | sort | tail -n1 || true)
  printf '%s %s %s\n' "$(printf '0%.0s' $(seq 1 40))" "$1" "$2" | \
    env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
        CKGIT_REPOSITORY_ROOT="$repos" CKGIT_CONTROL_SOCKET="$test_root/control.sock" "$CKGIT_POST_RECEIVE" ||
    fail "post-receive hook failed"
  after=$(ls "$state/ci/runs/demo" | sort | tail -n1)
  [ "$after" != "$before" ] || fail "no job was queued"
  printf '%s\n' "$after"
}
field() { sed -n "s/^$2=//p" "$state/ci/runs/demo/$1/run.ini" 2>/dev/null || true; }
# Waits until run $1 has status $2 (seconds: $3).
wait_status() {
  attempt=0
  while [ "$(field "$1" status)" != "$2" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt "$(( $3 * 10 ))" ] || fail "run $1 never reached $2 (status: $(field "$1" status))"
    sleep .1
  done
}
# The runner empties its build root right after it reported the result, so
# give it a moment once the server shows the run finished.
build_root_empty() {
  attempt=0
  while [ -n "$(ls -A "$secondary_build" 2>/dev/null || true)" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 150 ] || fail "$1: the secondary kept data: $(ls -A "$secondary_build")"
    sleep .1
  done
}

cat > "$test_root/server.ini" <<INI
schema_version=1
repo_root=$repos
control_socket=$test_root/control.sock
state_root=$state
ci_build_root=$test_root/primary-build
pages_root=$test_root/pages
ci_lease_seconds=10
ci_claim_delay_ms=1000
INI
"$CKGIT_ADMIN" ci enable demo --config "$test_root/server.ini" >/dev/null || fail "ci enable failed"

# The dashboard, for the live log stream.
"$CKGIT_HOSTINGD" --repo-root "$repos" --state-root "$state" --control-socket "$test_root/control.sock" \
  --http-port 0 > "$test_root/server.log" 2>&1 &
server_pid=$!
port=''
attempt=0
while [ -z "$port" ]; do
  kill -0 "$server_pid" 2>/dev/null || fail "daemon exited"
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$test_root/server.log" 2>/dev/null || true)
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail "daemon listener never became ready"
  [ -n "$port" ] || sleep .1
done

# The stand-in for ssh. Marker files make it misbehave once, on purpose:
#   drop-put     -- the first `put` fails as if the connection dropped;
#   cut-stream   -- the first `stream` session is killed after two seconds.
cat > "$test_root/fake-ssh" <<EOF
#!/bin/sh
for last; do :; done
SSH_ORIGINAL_COMMAND=\$last
export SSH_ORIGINAL_COMMAND
case "\$last" in
  *" put "*)
    if [ -e "$test_root/marks/drop-put" ]; then rm -f "$test_root/marks/drop-put"; exit 255; fi ;;
  *" stream "*)
    if [ -e "$test_root/marks/cut-stream" ]; then
      rm -f "$test_root/marks/cut-stream"
      ( sleep 2; kill -9 \$\$ ) &
    fi ;;
esac
exec "$CK_GIT_SHELL" --client-id uconsole --repo-root "$repos" --control-socket "$test_root/control.sock" \\
  --state-root "$state" --runner-config "$test_root/server.ini" --runner-program "$CK_CI_RUNNER"
EOF
chmod 0755 "$test_root/fake-ssh"
cat > "$test_root/runner.ini" <<INI
schema_version=1
server=ckgit@main-host
name=uconsole
ci_build_root=$secondary_build
ssh_program=$test_root/fake-ssh
claim_wait_seconds=5
ci_allow_degraded=true
INI
start_primary() {
  "$CK_CI_RUNNER" serve --config "$test_root/server.ini" >> "$test_root/primary.log" 2>&1 &
  primary_pid=$!
}
start_secondary() {
  "$CK_CI_RUNNER" serve --remote --config "$test_root/runner.ini" >> "$test_root/secondary.log" 2>&1 &
  secondary_pid=$!
  sleep 1  # let its first long poll reach the gateway
}

# --- 1: an idle secondary wins; history depth and a pinned sister ----------
start_primary
start_secondary
quick=$(publish "version: 1
checkout: { depth: 5 }
sisters:
  - name: lib
    ref: $lib_pin
    depth: 2
jobs:
  - name: build
    steps:
      - name: check
        script: |
          echo first-job
          test \"\$(git rev-list --count HEAD)\" = 5
          test \"\$(git -C ../lib rev-parse HEAD)\" = $lib_pin
          test \"\$(git -C ../lib rev-list --count HEAD)\" = 2
          grep -q 'lib 3' ../lib/version.txt")
queued_at=$(date +%s)
run1=$(queue "$quick" refs/heads/main)
wait_status "$run1" success 30
[ "$(field "$run1" runner)" = uconsole ] || fail "the idle secondary did not take the fresh job (runner $(field "$run1" runner))"
started=$(field "$run1" started_epoch)
[ $((started - queued_at)) -le 3 ] || fail "the job started $((started - queued_at))s after it was queued"
build_root_empty "after a successful job"

# --- 2: without a free secondary the primary takes the job ------------------
stop_pid "$secondary_pid"; secondary_pid=''
run2=$(queue "$quick" refs/heads/main)
wait_status "$run2" success 30
[ "$(field "$run2" runner)" = primary ] || fail "the primary did not take the job (runner $(field "$run2" runner))"
stop_pid "$primary_pid"; primary_pid=''

# --- 3: a stream cut mid-step resumes without duplicate lines; live SSE ------
slow=$(publish "version: 1
jobs:
  - name: slow
    steps:
      - name: count
        script: |
          i=1
          while [ \$i -le 12 ]; do echo line-\$i; i=\$((i + 1)); sleep .5; done")
: > "$test_root/marks/cut-stream"
start_secondary
run3=$(queue "$slow" refs/heads/main)
wait_status "$run3" running 20
curl --silent --max-time 4 -N "http://127.0.0.1:$port/project/demo/ci/$run3/0.stream" > "$test_root/sse.txt" || true
wait_status "$run3" success 40
grep -q '^data: line-' "$test_root/sse.txt" || fail "the dashboard's live stream did not follow the remote run"
for n in 1 2 3 4 5 6 7 8 9 10 11 12; do
  count=$(grep -c "^line-$n\$" "$state/ci/runs/demo/$run3/steps/0.log" || true)
  [ "$count" = 1 ] || fail "line-$n appears $count times after the stream was cut and resumed"
done
build_root_empty "after a resumed stream"

# --- 4: a cancel reaches the remote run -------------------------------------
run4=$(queue "$slow" refs/heads/main)
wait_status "$run4" running 20
"$CKGIT_ADMIN" ci cancel demo "$run4" --config "$test_root/server.ini" >/dev/null || fail "ci cancel failed"
wait_status "$run4" cancelled 20
[ "$(field "$run4" runner)" = uconsole ] || fail "the cancelled run was not the secondary's"
sleep 1
build_root_empty "after a cancelled job"

# --- 5: a tag build publishes a release; the first upload is dropped --------
release=$(publish "version: 1
on: { branches: [main], tags: ['v*'] }
jobs:
  - name: package
    steps:
      - run: sh -ec 'mkdir -p dist && dd if=/dev/zero of=dist/pkg.bin bs=1000 count=300 2>/dev/null'
    artifacts:
      name: pkg
      paths: [dist]")
git -C "$work" tag v1.0
git -C "$work" push -q "$repos/demo.git" v1.0
: > "$test_root/marks/drop-put"
run5=$(queue "$release" refs/tags/v1.0)
wait_status "$run5" success 60
[ ! -e "$test_root/marks/drop-put" ] || fail "the dropped upload was never attempted"
[ -f "$state/releases/demo/v1.0/release.ini" ] || fail "the tag build published no release"
[ -f "$state/releases/demo/v1.0/pkg.tar" ] || fail "the release lacks its asset"
tar -xOf "$state/releases/demo/v1.0/pkg.tar" dist/pkg.bin | wc -c | grep -q 300000 ||
  fail "the release asset is not complete after the resumed upload"
build_root_empty "after a release build"

# --- 6: a secondary killed mid-run; the primary completes attempt 2 ---------
run6=$(queue "$slow" refs/heads/main)
wait_status "$run6" running 20
kill -9 "$secondary_pid" 2>/dev/null || true
wait "$secondary_pid" 2>/dev/null || true
secondary_pid=''
start_primary
attempt=0
until [ "$(field "$run6" status)" = success ] && [ "$(field "$run6" attempt)" = 2 ]; do
  attempt=$((attempt + 1))
  [ "$attempt" -lt 600 ] || fail "the lost run was not requeued and completed (status $(field "$run6" status), attempt $(field "$run6" attempt))"
  sleep .1
done
[ "$(field "$run6" runner)" = primary ] || fail "attempt 2 did not run on the primary"
[ -d "$state/ci/runs/demo/$run6/attempt-1" ] || fail "the lost attempt's logs were not kept aside"
stop_pid "$primary_pid"; primary_pid=''
# The restarted secondary empties what the killed one left behind.
"$CK_CI_RUNNER" serve --remote --config "$test_root/runner.ini" --once >> "$test_root/secondary.log" 2>&1 ||
  fail "the secondary did not restart"
build_root_empty "after a restart following a kill"

# --- 7: a primary stopped mid-run hands the job back -------------------------
start_primary
run7=$(queue "$slow" refs/heads/main)
wait_status "$run7" running 20
[ "$(field "$run7" runner)" = primary ] || fail "the stopped run was not the primary's"
sleep 1
stop_pid "$primary_pid"; primary_pid=''
[ "$(field "$run7" status)" = pending ] ||
  fail "a stopped primary did not hand its job back (status $(field "$run7" status))"
[ "$(field "$run7" attempt)" = 2 ] || fail "the handed-back job is not on its next attempt"
grep -q "demo $run7 handed back" "$test_root/primary.log" || fail "the primary did not report the hand-back"
[ -f "$state/ci/spool/$run7.ini" ] || fail "the handed-back job is not queued again"
start_secondary
wait_status "$run7" success 40
[ "$(field "$run7" runner)" = uconsole ] && [ "$(field "$run7" attempt)" = 2 ] ||
  fail "the handed-back job did not complete on the secondary as attempt 2"
stop_pid "$secondary_pid"; secondary_pid=''
build_root_empty "after a handed-back job"

echo "shared runners integration test passed"
