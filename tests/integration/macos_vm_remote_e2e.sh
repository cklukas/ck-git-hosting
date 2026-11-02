#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Opt-in integration: real VM and gateway, with SSH replaced by a local shim.
# All paths must be new, task-owned and explicitly selected by the operator.
set -eu
if [ "$#" -ne 4 ]; then
  printf '%s\n' 'usage: macos_vm_remote_e2e.sh BIN-DIR PREPARED-BASE EMPTY-VM-WORK-ROOT NEW-FIXTURE-ROOT' >&2
  exit 2
fi
bin=$1
base=$2
vm_work=$3
fixture=$4
case "$bin:$base:$vm_work:$fixture" in /*:/*:/*:/*) ;; *) printf '%s\n' 'all paths must be absolute' >&2; exit 1;; esac
[ ! -e "$fixture" ] && [ ! -L "$fixture" ] || { printf '%s\n' 'fixture root must be new' >&2; exit 1; }
[ -d "$vm_work" ] && [ -z "$(ls -A "$vm_work")" ] || { printf '%s\n' 'VM work root must be empty' >&2; exit 1; }
umask 077
mkdir "$fixture" "$fixture/tmp" "$fixture/repos" "$fixture/state" "$fixture/pages" "$fixture/runner"
TMPDIR="$fixture/tmp"; export TMPDIR
repo="$fixture/repos"; state="$fixture/state"; work="$fixture/work"; build="$fixture/runner/build"
mkdir -m 700 "$build"
git -c init.defaultBranch=main init -q "$work"
mkdir "$work/.ckgit"
cat > "$work/.ckgit/ci.yml" <<'YML'
version: 1
auto_cancel: false
jobs:
  - name: build
    steps:
      - name: compile
        script: |
          echo vm-remote-ok
          mkdir -p out
          printf vm-artifact > out/result.txt
    artifacts:
      name: vm-output
      paths: [out]
YML
git -C "$work" add .ckgit/ci.yml
git -C "$work" -c user.email=fixture@example.invalid -c user.name=Fixture commit -q -m fixture
unlabelled_commit=$(git -C "$work" rev-parse HEAD)
git -C "$repo" -c init.defaultBranch=main init --bare -q demo.git
git -C "$work" push -q "$repo/demo.git" main
cat > "$fixture/server.ini" <<EOF
schema_version=1
repo_root=$repo
control_socket=$fixture/control.sock
state_root=$state
pages_root=$fixture/pages
EOF
"$bin/ckgit-admin" ci enable demo --config "$fixture/server.ini" >/dev/null
printf '%s\n' 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOz6m3Iv7uX1rX5dJZp1fZ3v1nA0qkGqk1xQmvE7ZbBc vm-fixture' > "$fixture/key.pub"
"$bin/ckgit-admin" runner add vm-fixture --public-key "$fixture/key.pub" --config "$fixture/server.ini" \
  --shell "$bin/ck-git-shell" --labels macos-vm,arm64 >/dev/null
cat > "$fixture/fake-ssh" <<EOF
#!/bin/sh
for last; do :; done
SSH_ORIGINAL_COMMAND=\$last
export SSH_ORIGINAL_COMMAND
exec "$bin/ck-git-shell" --client-id vm-fixture --repo-root "$repo" --control-socket "$fixture/control.sock" \\
  --state-root "$state" --runner-config "$fixture/server.ini" --runner-program "$bin/ck-ci-runnerd"
EOF
chmod 700 "$fixture/fake-ssh"
cat > "$fixture/runner.ini" <<EOF
schema_version=1
server=ckgit@example.invalid
name=vm-fixture
ci_build_root=$build
ssh_program=$fixture/fake-ssh
claim_wait_seconds=1
ci_allow_network=false
ci_macos_vm_base=$base
ci_macos_vm_work_root=$vm_work
ci_macos_vm_min_free_gib=1
EOF
zero=0000000000000000000000000000000000000000
printf '%s %s refs/heads/main\n' "$zero" "$unlabelled_commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=fixture CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repo" "$bin/../hooks/post-receive"
unlabelled=$(find "$state/ci/spool" -name '*.ini' -type f -print)
[ -n "$unlabelled" ] && [ "$(printf '%s\n' "$unlabelled" | wc -l | tr -d ' ')" = 1 ] || {
  printf '%s\n' 'expected one unlabelled job' >&2; exit 1;
}
if ! "$bin/ck-ci-runnerd" serve --remote --config "$fixture/runner.ini" --once > "$fixture/runner.log" 2>&1; then
  cat "$fixture/runner.log" >&2; exit 1
fi
[ -f "$unlabelled" ] || { printf '%s\n' 'VM runner claimed an unlabelled job' >&2; exit 1; }
awk 'NR == 1 { print; print "runs_on: [macos-vm, arm64]"; next } { print }' \
  "$work/.ckgit/ci.yml" > "$fixture/labelled.yml"
mv "$fixture/labelled.yml" "$work/.ckgit/ci.yml"
git -C "$work" add .ckgit/ci.yml
git -C "$work" -c user.email=fixture@example.invalid -c user.name=Fixture commit -q -m labelled
commit=$(git -C "$work" rev-parse HEAD)
git -C "$work" push -q "$repo/demo.git" main
printf '%s %s refs/heads/main\n' "$unlabelled_commit" "$commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=fixture CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repo" "$bin/../hooks/post-receive"
if ! "$bin/ck-ci-runnerd" serve --remote --config "$fixture/runner.ini" --once > "$fixture/runner.log" 2>&1; then
  cat "$fixture/runner.log" >&2; exit 1
fi
run_id=$(awk '/ claimed / { print $3; exit }' "$fixture/runner.log")
run_ini="$state/ci/runs/demo/$run_id/run.ini"
[ -n "$run_ini" ] || { printf '%s\n' 'missing run record' >&2; exit 1; }
[ -n "$run_id" ] && [ -f "$unlabelled" ] || { printf '%s\n' 'labelled run missing or unlabelled job changed' >&2; exit 1; }
case "$(cat "$run_ini")" in *'status=success'*) ;; *) cat "$run_ini" >&2; exit 1;; esac
run_dir=${run_ini%/run.ini}
case "$(cat "$run_dir/steps/0.log")" in *vm-remote-ok*) ;; *) printf '%s\n' 'missing guest log' >&2; exit 1;; esac
[ "$(tar -xOf "$run_dir/artifacts/vm-output.tar" out/result.txt)" = vm-artifact ] || {
  printf '%s\n' 'missing guest artifact' >&2; exit 1;
}
[ -z "$(ls -A "$build")" ] || { printf '%s\n' 'runner relay was not cleaned' >&2; exit 1; }
[ "$(ls -A "$vm_work")" = '.ckgit-runner.lock' ] || { printf '%s\n' 'VM clone was not cleaned' >&2; exit 1; }
unlabelled_id=${unlabelled##*/}
unlabelled_id=${unlabelled_id%.ini}
"$bin/ckgit-admin" ci cancel demo "$unlabelled_id" --config "$fixture/server.ini" >/dev/null
"$bin/ckgit-admin" runner labels vm-fixture - --config "$fixture/server.ini" >/dev/null
"$bin/ck-ci-runnerd" serve --remote --config "$fixture/runner.ini" --once > "$fixture/final-drain.log" 2>&1 || {
  cat "$fixture/final-drain.log" >&2; exit 1;
}
[ -z "$(ls -A "$state/ci/spool")" ] || { printf '%s\n' 'fixture queue was not drained' >&2; exit 1; }
printf '%s\n' 'VM remote routing, gateway and artifact integration passed'
