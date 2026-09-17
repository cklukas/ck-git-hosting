#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in "$test_root_parent"/*) ;; *) echo 'Select an approved TMPDIR' >&2; exit 1 ;; esac
[ -d "$test_root_parent" ]
test_root=$(mktemp -d "$test_root_parent/cks.XXXXXX")
cleanup() {
  if [ -f "$test_root/tunnel.pid" ]; then
    kill -TERM "$(cat "$test_root/tunnel.pid")" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$test_root/tmp" "$test_root/bin" "$test_root/repos" "$test_root/projects"
TMPDIR="$test_root/tmp"
TEST_ROOT="$test_root"
REAL_GIT=$(command -v git)
export TMPDIR TEST_ROOT REAL_GIT
fail() { echo "setup/doctor: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) ;; *) fail "expected '$2': $1" ;; esac; }
capture() {
  set +e
  output=$("$@" 2>&1)
  result=$?
  set -e
}
setup() {
  "$CKGIT" setup --config "$test_root/config/client.ini" --server ckgit@fixture \
    --client-id setup-test --display-name 'Setup Test' --remote-name ckgit \
    --web-host admin@fixture --remote-port 8422 --scan-root "$test_root/projects" "$@"
}

# Setup previews are offline and create neither a file nor its parent folder.
capture setup --dry-run
[ "$result" -eq 0 ] || fail "$output"
contains "$output" 'server=ckgit@fixture'
contains "$output" 'web_host=admin@fixture'
contains "$output" 'web_port=8422'
contains "$output" 'public_path_mode=basename'
[ ! -e "$test_root/config" ] || fail 'dry run created configuration directories'
capture setup --yes
[ "$result" -eq 0 ] || fail "$output"
contains "$output" 'Saved '
contains "$output" 'ckgit doctor --config'
contains "$output" 'IdentityFile'
cp "$test_root/config/client.ini" "$test_root/original.ini"
capture setup --server ckgit@different --yes
[ "$result" -eq 2 ] || fail 'existing configuration was not protected'
cmp "$test_root/config/client.ini" "$test_root/original.ini" || fail 'refused setup changed existing config'
printf '%s\n' 'exclude=*/node_modules/*' >>"$test_root/config/client.ini"
capture "$CKGIT" setup --config "$test_root/config/client.ini" --overwrite --display-name 'Renamed device' --yes
[ "$result" -eq 0 ] || fail "$output"
grep -Fq 'display_name=Renamed device' "$test_root/config/client.ini"
grep -Fq 'exclude=*/node_modules/*' "$test_root/config/client.ini"
grep -Fq "scan_root=$test_root/projects" "$test_root/config/client.ini"
cp "$test_root/config/client.ini" "$test_root/saved.ini"
capture "$CKGIT" setup --config "$test_root/config/client.ini" --overwrite --server invalid --yes
[ "$result" -eq 2 ] || fail 'invalid replacement was accepted'
cmp "$test_root/config/client.ini" "$test_root/saved.ini" || fail 'invalid replacement changed configuration'
capture "$CKGIT" setup --config "$test_root/preview/client.ini" --server ckgit@fixture --yes --dry-run
[ "$result" -eq 0 ] || fail "$output"
[ ! -e "$test_root/preview" ] || fail '--dry-run did not override --yes'

# Exercise real terminal prompting without depending on the user's terminal.
run_tty() {
  if [ "$(uname -s)" = Darwin ]; then
    script -q "$test_root/terminal.typescript" "$CKGIT" setup "$@"
  else
    command_line=''
    for argument in "$CKGIT" setup "$@"; do
      quoted=$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")
      command_line="$command_line '$quoted'"
    done
    script -q -e -c "$command_line" "$test_root/terminal.typescript"
  fi
}
# BSD script forwards pipe EOF as a terminal EOF character. Give its child
# time to open the terminal, then keep input open while it consumes the answers.
terminal_answers() {
  sleep .2
  printf '%b' "$1"
  sleep 1
}
terminal_answers 'ckgit@fixture\nGuided device\ny\n' | run_tty --config "$test_root/guided/client.ini" \
  --client-id guided --remote-name ckgit --web-host admin@fixture --remote-port 8422 \
  --public-path-mode basename --scan-root "$test_root/projects" >"$test_root/guided.log" 2>&1 || {
    cat "$test_root/guided.log" >&2; fail 'guided setup failed';
  }
grep -Fq 'display_name=Guided device' "$test_root/guided/client.ini"
terminal_answers 'n\n' | run_tty --config "$test_root/cancelled/client.ini" --server ckgit@fixture \
  --client-id cancelled --display-name Cancelled --remote-name ckgit --web-host admin@fixture \
  --remote-port 8422 --public-path-mode basename --scan-root "$test_root/projects" >"$test_root/cancelled.log" 2>&1
[ ! -e "$test_root/cancelled" ] || fail 'declining setup created configuration'

# The SSH fixture serves real HTTP on the requested loopback forwarding port.
# It validates that control uses the restricted account and the dashboard uses
# the separately configured administrative target. No external host is used.
cat >"$test_root/bin/ssh" <<'SH'
#!/bin/sh
set -eu
last='' target='' forward=''
for argument do
  case "$argument" in
    ckgit@fixture|admin@fixture|admin@override) target=$argument ;;
    127.0.0.1:*:127.0.0.1:*) forward=$argument ;;
  esac
  last=$argument
done
printf '%s\n' "$*" >>"$TEST_ROOT/ssh.log"
if [ "$last" = 'ckgit-rpc 1 ping' ]; then
  [ "$target" = ckgit@fixture ] || exit 91
  if [ "${DOCTOR_CASE:-}" = git-offline ]; then echo 'Permission denied (publickey)' >&2; exit 255; fi
  printf 'ok\n'
  exit 0
fi
if [ "$last" = 'ckgit-rpc 1 version' ]; then
  [ "$target" = ckgit@fixture ] || exit 91
  printf 'ok 0.1.0+fixture\n'
  exit 0
fi
[ "$target" = admin@fixture ] || [ "$target" = admin@override ] || exit 92
[ -n "$forward" ] || exit 93
if [ "${DOCTOR_CASE:-}" = web-offline ]; then echo 'administrative SSH unavailable' >&2; exit 255; fi
printf '%s\n' "$$" >"$TEST_ROOT/tunnel.pid"
if [ "${DOCTOR_CASE:-}" = web-timeout ]; then trap '' TERM; exec sleep 30; fi
port=${forward#127.0.0.1:}; port=${port%%:*}
if [ "${DOCTOR_CASE:-}" = web-slow ]; then
  # Bytes keep arriving, so a per-read timeout would never bound this check.
  while :; do printf H; sleep .1; done | nc -lk 127.0.0.1 "$port"
  exit
fi
exec "$CKGIT_HOSTINGD" --repo-root "$TEST_ROOT/repos" --control-socket "$TEST_ROOT/dashboard.sock" --http-port "$port"
SH
cat >"$test_root/bin/git" <<'SH'
#!/bin/sh
if [ "${DOCTOR_CASE:-}" = git-missing ]; then echo 'Git unavailable in fixture' >&2; exit 127; fi
exec "$REAL_GIT" "$@"
SH
chmod 0700 "$test_root/bin/ssh" "$test_root/bin/git"
PATH="$test_root/bin:$PATH"
export PATH
doctor() { "$CKGIT" doctor --config "$test_root/config/client.ini" --timeout 2 "$@"; }
capture doctor
[ "$result" -eq 0 ] || fail "$output"
contains "$output" 'OK   Configuration:'
contains "$output" 'OK   Git:'
contains "$output" 'OK   Git server: restricted SSH control answered on ckgit@fixture (server 0.1.0+fixture)'
contains "$output" 'OK   Dashboard: HTTP answered through admin@fixture on server port 8422'
contains "$output" 'All checks passed'
if kill -0 "$(cat "$test_root/tunnel.pid")" 2>/dev/null; then fail 'doctor left its tunnel running'; fi
grep -Fq 'StrictHostKeyChecking=yes' "$test_root/ssh.log"
grep -Fq 'UpdateHostKeys=no' "$test_root/ssh.log"
grep -Fq 'ckgit-rpc 1 ping' "$test_root/ssh.log"
grep -Fq 'ckgit-rpc 1 version' "$test_root/ssh.log"
capture doctor --web-host admin@override --remote-port 9000
[ "$result" -eq 0 ] || fail "$output"
contains "$output" 'HTTP answered through admin@override on server port 9000'
for failure in git-offline web-offline web-timeout web-slow git-missing; do
  DOCTOR_CASE=$failure
  export DOCTOR_CASE
  started=$(date +%s)
  capture doctor --timeout 1
  elapsed=$(( $(date +%s) - started ))
  [ "$result" -eq 1 ] || fail "doctor reported success for $failure: $output"
  contains "$output" 'FAIL '
  contains "$output" 'Next:'
  contains "$output" 'Some checks need attention'
  [ "$elapsed" -le 5 ] || fail "doctor did not respect its bounded check timeout for $failure"
  if [ -f "$test_root/tunnel.pid" ] && kill -0 "$(cat "$test_root/tunnel.pid")" 2>/dev/null; then
    fail "doctor left a tunnel after $failure"
  fi
done
unset DOCTOR_CASE
capture "$CKGIT" doctor --config "$test_root/not-found.ini" --web-host admin@fixture --timeout 2
[ "$result" -eq 1 ] || fail 'doctor reported success for missing configuration'
contains "$output" 'FAIL Configuration:'
contains "$output" 'OK   Git:'
contains "$output" 'OK   Dashboard:'
cmp "$test_root/config/client.ini" "$test_root/saved.ini" || fail 'doctor changed configuration'
[ ! -e "$test_root/config/sync.lock" ] || fail 'read-only diagnostics created an inventory lock'
echo 'setup and doctor integration tests passed'
