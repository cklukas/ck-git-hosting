#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end remote runner: a CI-enabled push queues a job on the "main host";
# a remote runner (`ck-ci-runnerd serve --remote`) claims it through the
# runner key's forced command (ck-git-shell in its runner role, then the
# gateway), fetches the source as a shallow Git clone through the gateway,
# runs it, relays its log live, uploads its artifact and Pages site, and
# finishes it -- and keeps nothing afterwards. A stand-in for ssh hands each
# remote command straight to ck-git-shell, so no sshd is needed.

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckrr.XXXXXX")
cleanup() { rm -rf "$test_root"; }
trap cleanup EXIT HUP INT TERM
fail() {
  echo "$1" >&2
  [ -f "$test_root/runner.log" ] && sed 's/^/runner: /' "$test_root/runner.log" >&2
  exit 1
}

: "${CK_GIT_SHELL:?CK_GIT_SHELL must point at ck-git-shell}"
: "${CKGIT_ADMIN:?CKGIT_ADMIN must point at ckgit-admin}"
: "${CK_CI_RUNNER:?CK_CI_RUNNER must point at ck-ci-runnerd}"
: "${CKGIT_POST_RECEIVE:?CKGIT_POST_RECEIVE must point at the post-receive hook}"

repos="$test_root/repos"
state="$test_root/state"
work="$test_root/work"
runner_build="$test_root/runner/build"
mkdir -p "$repos" "$test_root/runner" "$test_root/pages"
mkdir -p "$state"; chmod 700 "$state"

git -c init.defaultBranch=main init -q "$work"
mkdir -p "$work/.ckgit"
cat > "$work/.ckgit/ci.yml" <<'YML'
version: 1
jobs:
  - name: build
    steps:
      - name: compile
        script: |
          echo remote-ci-ok
          test -d .git
          test "$(git rev-parse HEAD)" = "$CKGIT_COMMIT"
          test "$(git rev-list --count HEAD)" = 1
          mkdir -p out public
          printf payload > out/artifact.txt
          printf remote-site > public/index.html
    artifacts:
      name: build
      paths: [out]
pages:
  path: public
YML
git -C "$work" add -A
git -C "$work" -c user.email=t@example.invalid -c user.name=Test commit -q -m first
git -C "$work" -c user.email=t@example.invalid -c user.name=Test commit -q --allow-empty -m second
commit=$(git -C "$work" rev-parse HEAD)
git -C "$repos" -c init.defaultBranch=main init --bare -q demo.git
git -C "$work" push -q "$repos/demo.git" main

cat > "$test_root/server.ini" <<INI
schema_version=1
repo_root=$repos
control_socket=$test_root/control.sock
state_root=$state
pages_root=$test_root/pages
INI
"$CKGIT_ADMIN" ci enable demo --config "$test_root/server.ini" >/dev/null || fail "ci enable failed"

# Register the runner; its key's forced command is exactly what ckgit-admin
# prints.
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOz6m3Iv7uX1rX5dJZp1fZ3v1nA0qkGqk1xQmvE7ZbBc uconsole\n' > "$test_root/key.pub"
line=$("$CKGIT_ADMIN" runner add uconsole --public-key "$test_root/key.pub" --config "$test_root/server.ini" \
  --shell "$CK_GIT_SHELL" 2>/dev/null) || fail "runner add failed"
case "$line" in
  *"--client-id uconsole"*"--runner-config $test_root/server.ini"*) ;;
  *) fail "the runner key line lacks the runner role: $line" ;;
esac
"$CKGIT_ADMIN" runner list --config "$test_root/server.ini" | grep -q '^uconsole  enabled, last seen never' ||
  fail "runner list does not show the registered runner"
"$CKGIT_ADMIN" runner add primary --public-key "$test_root/key.pub" --config "$test_root/server.ini" >/dev/null 2>&1 &&
  fail "the primary's name was accepted for a remote runner"

# The stand-in for ssh: its last argument is the remote command line, which
# sshd would hand to the forced command as SSH_ORIGINAL_COMMAND.
cat > "$test_root/fake-ssh" <<EOF
#!/bin/sh
for last; do :; done
SSH_ORIGINAL_COMMAND=\$last
export SSH_ORIGINAL_COMMAND
exec "$CK_GIT_SHELL" --client-id uconsole --repo-root "$repos" --control-socket "$test_root/control.sock" \\
  --state-root "$state" --runner-config "$test_root/server.ini" --runner-program "$CK_CI_RUNNER"
EOF
chmod 0755 "$test_root/fake-ssh"

# A device key cannot run runner operations, and a runner key cannot run Git.
SSH_ORIGINAL_COMMAND='ckgit-runner 1 claim 0' "$CK_GIT_SHELL" --client-id mac --repo-root "$repos" \
  --control-socket "$test_root/control.sock" --state-root "$state" >/dev/null 2>&1 &&
  fail "a device key ran a runner operation"
SSH_ORIGINAL_COMMAND="git-upload-pack 'demo.git'" "$CK_GIT_SHELL" --client-id uconsole --repo-root "$repos" \
  --control-socket "$test_root/control.sock" --state-root "$state" --runner-config "$test_root/server.ini" \
  --runner-program "$CK_CI_RUNNER" >/dev/null 2>&1 && fail "a runner key ran git-upload-pack directly"

cat > "$test_root/runner.ini" <<INI
schema_version=1
server=ckgit@main-host
name=uconsole
ci_build_root=$runner_build
ssh_program=$test_root/fake-ssh
claim_wait_seconds=1
ci_allow_degraded=true
INI

# Queue the job like a push does.
printf '%s %s refs/heads/main\n' "$(printf '0%.0s' $(seq 1 40))" "$commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive hook failed"

# A disabled (draining) runner leaves the job queued.
"$CKGIT_ADMIN" runner disable uconsole --config "$test_root/server.ini" >/dev/null || fail "runner disable failed"
"$CK_CI_RUNNER" serve --remote --config "$test_root/runner.ini" --once > "$test_root/runner.log" 2>&1 ||
  fail "the disabled runner failed"
ls "$state"/ci/spool/*.ini >/dev/null 2>&1 || fail "a disabled runner took a job"
"$CKGIT_ADMIN" runner enable uconsole --config "$test_root/server.ini" >/dev/null || fail "runner enable failed"

"$CK_CI_RUNNER" serve --remote --config "$test_root/runner.ini" --once > "$test_root/runner.log" 2>&1 ||
  fail "the remote runner failed"

run_ini=$(ls "$state"/ci/runs/demo/*/run.ini 2>/dev/null | head -n1 || true)
[ -n "$run_ini" ] || fail "no run record was produced"
grep -q "status=success" "$run_ini" || { cat "$run_ini" >&2; fail "the remote run did not succeed"; }
run_dir=$(dirname "$run_ini")
grep -q "remote-ci-ok" "$run_dir/steps/0.log" || fail "the step log was not relayed to the main host"
[ -f "$run_dir/artifacts/build.tar" ] || fail "the artifact was not uploaded"
tar -tf "$run_dir/artifacts/build.tar" | grep -q 'out/artifact.txt' || fail "the uploaded artifact lacks the built file"
[ "$(cat "$test_root/pages/demo/current" 2>/dev/null)" = "$(basename "$run_dir")" ] ||
  fail "the Pages site was not published from the remote run"
grep -rq "remote-site" "$test_root/pages/demo/versions" || fail "the published site lacks the built page"
[ ! -d "$run_dir/incoming" ] || fail "the staging was not cleaned up"
[ -z "$(ls -A "$state/ci/working" 2>/dev/null || true)" ] || fail "the job was not released"
[ -z "$(ls -A "$runner_build" 2>/dev/null || true)" ] || fail "the runner kept data after the job: $(ls -A "$runner_build")"
grep -q "ci-claimed" "$state"/events/*.log 2>/dev/null || fail "the claim was not recorded as an event"
"$CKGIT_ADMIN" runner list --config "$test_root/server.ini" | grep -q '^uconsole  enabled, last seen [0-9]*s ago' ||
  fail "runner list does not show the runner's last contact"

echo "remote runner integration test passed"
