#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds this repository's own documentation with the ckdocs binary and
# checks the result the way a reader's browser would: the three top-level
# tabs are present, no absolute href/src leaked in, and every internal
# reference resolves to a file relative to the page that carries it. Then, on
# a small synthetic fixture, proves a broken link fails `ckdocs check` with a
# report naming both the offending page and its target -- the CLI wiring
# that unit tests, which call the library directly, cannot reach. Finally,
# `ckdocs serve` is started on an OS-chosen loopback port and probed for the
# index, a nested page, and a 404, then stopped.

set -eu

: "${CKDOCS:?CKDOCS must name the built ckdocs binary}"
[ -x "$CKDOCS" ] || { echo "CKDOCS=$CKDOCS is not executable" >&2; exit 1; }

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
[ -d "$test_root_parent" ] || { echo "Approved temporary volume is unavailable" >&2; exit 1; }
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckdocs-site.XXXXXX")
serve_pid=''
cleanup() {
  [ -z "$serve_pid" ] || { kill "$serve_pid" 2>/dev/null || true; wait "$serve_pid" 2>/dev/null || true; }
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

# ---- this repository's own site --------------------------------------------

site="$test_root/site"
if ! "$CKDOCS" build --root "$repo_root" --out "$site" --strict >"$test_root/build.log" 2>&1; then
  echo "ckdocs build --strict failed on this repository's own documentation:" >&2
  cat "$test_root/build.log" >&2
  exit 1
fi

for page in index.html operations/01-installation.html protocol/01-ssh-and-control-v1.html site-index.html .ckdocs; do
  [ -f "$site/$page" ] || { echo "expected $page in the built site" >&2; exit 1; }
done

for tab in Home Operations Protocol; do
  grep -q ">$tab</a>" "$site/index.html" || { echo "index.html is missing the $tab tab" >&2; exit 1; }
done

# Restricted to *.html: the site legitimately carries copied non-HTML
# assets (this guide links to this very script, which is why it is one),
# and this check must not flag literal shell text in a copied source file.
if find "$site" -name '*.html' -exec grep -l 'href="/[^/]\|src="/[^/]' {} + >/dev/null 2>&1; then
  echo "an absolute href or src leaked into the built site:" >&2
  find "$site" -name '*.html' -exec grep -n 'href="/[^/]\|src="/[^/]' {} + >&2
  exit 1
fi

# Every internal href/src resolves to a real file, relative to the page that
# carries it (percent-encoding is not decoded here, since none of this
# repository's own filenames need it).
fail=0
pages_list=$(mktemp "$test_root/pages.XXXXXX")
refs_list=$(mktemp "$test_root/refs.XXXXXX")
find "$site" -name '*.html' >"$pages_list"
while IFS= read -r page; do
  page_rel=${page#"$site"/}
  page_dir=$(dirname "$page_rel")
  grep -oE '(href|src)="[^"]+"' "$page" >"$refs_list" || true
  while IFS= read -r raw; do
    [ -n "$raw" ] || continue
    target=${raw#*=\"}
    target=${target%\"}
    case "$target" in
      http://*|https://*|mailto:*|'#'*) continue ;;
    esac
    target=${target%%#*}
    [ -n "$target" ] || continue
    target_dir=$(dirname "$target")
    target_base=$(basename "$target")
    resolved_dir=$(cd "$site/$page_dir/$target_dir" 2>/dev/null && pwd) || {
      echo "$page_rel: reference '$target' has no such directory" >&2
      fail=1
      continue
    }
    case "$resolved_dir" in
      "$site"|"$site"/*) ;;
      *) echo "$page_rel: reference '$target' resolves outside the site" >&2; fail=1; continue ;;
    esac
    [ -f "$resolved_dir/$target_base" ] || {
      echo "$page_rel: reference '$target' does not exist" >&2
      fail=1
    }
  done <"$refs_list"
done <"$pages_list"
[ "$fail" -eq 0 ] || { echo "docs_site link check failed" >&2; exit 1; }

# check builds into its own removed-afterward directory and agrees with build.
"$CKDOCS" check --root "$repo_root" >"$test_root/check.log" 2>&1 ||
  { echo "ckdocs check failed on this repository's own documentation:" >&2; cat "$test_root/check.log" >&2; exit 1; }

# A second build without --clean is refused; the first site is untouched.
if "$CKDOCS" build --root "$repo_root" --out "$site" >/dev/null 2>&1; then
  echo "a second build without --clean should have been refused" >&2
  exit 1
fi
[ -f "$site/index.html" ] || { echo "a refused rebuild must leave the existing site intact" >&2; exit 1; }

# ---- a synthetic fixture with a broken link --------------------------------

fixture="$test_root/fixture"
mkdir -p "$fixture/docs"
cat >"$fixture/README.md" <<'EOF'
# Fixture

See the [guide](docs/guide.md) and a [broken link](docs/missing.md).
EOF
cat >"$fixture/docs/guide.md" <<'EOF'
# Guide

Body.
EOF

set +e
fixture_output=$("$CKDOCS" check --root "$fixture" 2>&1)
fixture_status=$?
set -e
[ "$fixture_status" -eq 1 ] || {
  echo "ckdocs check on a broken fixture should exit 1, got $fixture_status:" >&2
  echo "$fixture_output" >&2
  exit 1
}
case "$fixture_output" in
  *"README.md"*"docs/missing.md"*) ;;
  *)
    echo "ckdocs check's report should name both the page and the broken target:" >&2
    echo "$fixture_output" >&2
    exit 1
    ;;
esac
[ ! -e "$fixture/public" ] || { echo "check must never leave an output directory behind" >&2; exit 1; }

# ---- serve: a loopback preview server --------------------------------------

serve_log="$test_root/serve.log"
"$CKDOCS" serve --root "$repo_root" --port 0 --quiet >"$serve_log" 2>&1 &
serve_pid=$!

serve_port=''
attempt=0
while [ -z "$serve_port" ]; do
  if ! kill -0 "$serve_pid" 2>/dev/null; then
    echo "ckdocs serve exited before it became ready:" >&2
    cat "$serve_log" >&2
    serve_pid=''
    exit 1
  fi
  serve_port=$(sed -n 's/^ckdocs: serve ready on \([0-9][0-9]*\)$/\1/p' "$serve_log")
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ]; then
    echo "ckdocs serve never printed its ready banner:" >&2
    cat "$serve_log" >&2
    exit 1
  fi
  [ -n "$serve_port" ] || sleep .1
done

check_serve() {  # check_serve PATH 'HTTP STATUS LINE' [CONTENT-TYPE PATTERN]
  headers="$test_root/serve-headers"
  curl --max-time 4 --silent --show-error -D "$headers" -o "$test_root/serve-body" \
    "http://127.0.0.1:$serve_port$1"
  grep -qF "$2" "$headers" || {
    echo "serving $1: expected '$2' in the response headers:" >&2
    cat "$headers" >&2
    exit 1
  }
  if [ $# -ge 3 ]; then
    grep -qi "^Content-Type: $3" "$headers" || {
      echo "serving $1: expected a Content-Type matching '$3':" >&2
      cat "$headers" >&2
      exit 1
    }
  fi
}

check_serve "/" "HTTP/1.1 200 OK" "text/html"
check_serve "/operations/07-docs-sites.html" "HTTP/1.1 200 OK" "text/html"
check_serve "/nope.html" "HTTP/1.1 404 Not Found"

kill "$serve_pid" 2>/dev/null || true
wait "$serve_pid" 2>/dev/null || true
serve_pid=''
echo "docs_site serve check passed"

echo "docs_site integration test passed"
