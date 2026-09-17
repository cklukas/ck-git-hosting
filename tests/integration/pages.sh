#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end Pages: a CI-enabled push builds a static site, the runner publishes
# it, and the separate ck-pagesd serves it on its own port with traversal denied.

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckpages.XXXXXX")
pages_pid=''
cleanup() {
  if [ -n "$pages_pid" ]; then
    kill -TERM "$pages_pid" 2>/dev/null || true
    wait "$pages_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
fail() { echo "$1" >&2; exit 1; }

: "${CKGIT_ADMIN:?CKGIT_ADMIN must point at ckgit-admin}"
: "${CK_CI_RUNNER:?CK_CI_RUNNER must point at ck-ci-runnerd}"
: "${CK_PAGES:?CK_PAGES must point at ck-pagesd}"
: "${CKGIT_POST_RECEIVE:?CKGIT_POST_RECEIVE must point at the post-receive hook}"

repos="$test_root/repos"; state="$test_root/state"; build="$test_root/build"
pages="$test_root/pages"; work="$test_root/work"
mkdir -p "$repos" "$build" "$pages"
mkdir -p "$state"; chmod 700 "$state"

git -c init.defaultBranch=main init -q "$work"
mkdir -p "$work/.ckgit"
cat > "$work/.ckgit/ci.yml" <<'YML'
version: 1
pages: { path: public }
jobs:
  - name: site
    steps:
      - run: mkdir -p public && printf hello-pages > public/index.html && printf body-css > public/style.css
YML
git -C "$work" add -A
git -C "$work" -c user.email=t@example.invalid -c user.name=Test commit -q -m site
commit=$(git -C "$work" rev-parse HEAD)
git -C "$repos" -c init.defaultBranch=main init --bare -q demo.git
git -C "$work" push -q "$repos/demo.git" main

cat > "$test_root/server.ini" <<INI
schema_version=1
repo_root=$repos
control_socket=$test_root/control.sock
state_root=$state
ci_build_root=$build
pages_root=$pages
pages_http_port=0
INI

"$CKGIT_ADMIN" ci enable demo --config "$test_root/server.ini" >/dev/null || fail "ci enable failed"
printf '%s %s refs/heads/main\n' "$(printf '0%.0s' $(seq 1 40))" "$commit" | \
  env CKGIT_STATE_ROOT="$state" CKGIT_CLIENT_ID=mac-studio CKGIT_PROJECT_NAME=demo \
      CKGIT_REPOSITORY_ROOT="$repos" "$CKGIT_POST_RECEIVE" || fail "post-receive hook failed"
"$CK_CI_RUNNER" serve --config "$test_root/server.ini" --once >/dev/null 2>&1 || fail "serve failed"
[ -f "$pages/demo/current" ] || fail "the site was not published"

"$CK_PAGES" serve --config "$test_root/server.ini" > "$test_root/pages.log" 2>&1 &
pages_pid=$!
attempt=0; port=''
while [ -z "$port" ]; do
  kill -0 "$pages_pid" 2>/dev/null || { cat "$test_root/pages.log" >&2; fail "ck-pagesd exited"; }
  port=$(sed -n 's/^ck-pagesd: pages ready on \([0-9][0-9]*\)$/\1/p' "$test_root/pages.log")
  attempt=$((attempt + 1)); [ "$attempt" -lt 100 ] || fail "ck-pagesd never became ready"
  [ -n "$port" ] || sleep .1
done
base="http://127.0.0.1:$port"

curl --path-as-is --max-time 4 --silent -o "$test_root/home.html" "$base/demo/" || fail "curl home"
grep -q "hello-pages" "$test_root/home.html" || { cat "$test_root/home.html"; fail "index.html was not served"; }
curl --path-as-is --max-time 4 --silent -o "$test_root/style.css" "$base/demo/style.css" || fail "curl css"
grep -q "body-css" "$test_root/style.css" || fail "a nested asset was not served"

code=$(curl --path-as-is --max-time 4 --silent -o /dev/null -w '%{http_code}' "$base/demo/../../../etc/hostname" || true)
[ "$code" = "404" ] || fail "a traversal request was not refused (got $code)"
code=$(curl --path-as-is --max-time 4 --silent -o /dev/null -w '%{http_code}' "$base/absent/" || true)
[ "$code" = "404" ] || fail "an unknown project was not a 404 (got $code)"

echo "pages integration OK"
