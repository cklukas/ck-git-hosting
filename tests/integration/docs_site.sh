#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds this repository's own documentation with the ckdocs binary and
# checks the result the way a reader's browser would: its two sites (the
# ck-git-hosting manual and the ckdocs manual) carry their task-based tabs,
# logos and site selector, links between them resolve, the former addresses
# redirect, no absolute href/src leaked in, and every internal
# reference resolves to a file relative to the page that carries it. Then, on
# a small synthetic fixture, proves a broken link fails `ckdocs check` with a
# report naming both the offending page and its target -- the CLI wiring
# that unit tests, which call the library directly, cannot reach -- and that
# a front matter key declared under front_matter.foreign_keys passes `ckdocs
# check` and reaches the page head while an undeclared one fails it, and
# that a portal (two sites, one from another Git branch) builds with its
# selector, welcome page and redirects, from the repository or with
# --ref-tree. Finally,
# `ckdocs serve` is started on an OS-chosen loopback port and probed for the
# index, a nested page, and a 404, then stopped.

set -eu
. "$(dirname "$0")/stop_process.sh"

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
# Stops the running `ckdocs serve` (see stop_process.sh: never an unbounded
# wait); fails when it had to be killed.
stop_serve() {
  [ -n "$serve_pid" ] || return 0
  stop_serve_status=0
  stop_process "$serve_pid" || stop_serve_status=1
  serve_pid=''
  return "$stop_serve_status"
}
# Waits for `ckdocs serve` writing to log $1 to announce its port; sets
# serve_port. The log appears only once the background job has opened it.
await_serve() {
  serve_port=''
  ready_attempt=0
  while [ -z "$serve_port" ]; do
    if ! kill -0 "$serve_pid" 2>/dev/null; then
      cat "$1" >&2 2>/dev/null || true
      serve_pid=''
      echo "ckdocs serve exited before it became ready" >&2
      exit 1
    fi
    [ ! -f "$1" ] || serve_port=$(sed -n 's/^ckdocs: serve ready on \([0-9][0-9]*\)$/\1/p' "$1")
    ready_attempt=$((ready_attempt + 1))
    if [ "$ready_attempt" -ge 100 ]; then
      cat "$1" >&2 2>/dev/null || true
      echo "ckdocs serve never printed its ready banner" >&2
      exit 1
    fi
    [ -n "$serve_port" ] || sleep .1
  done
}
cleanup() {
  stop_serve || true
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

# A publishing host override must change both indexes without editing config.
"$CKDOCS" build --root "$repo_root" --site-url https://publish.example.test/project --out "$test_root/host-site" --strict >"$test_root/host-build.log" 2>&1
grep -qF '(https://publish.example.test/project/hosting/index.html.md)' "$test_root/host-site/llms.txt" ||
  { echo "--site-url did not override llms.txt URLs" >&2; exit 1; }
grep -qF '<loc>https://publish.example.test/project/index.html</loc>' "$test_root/host-site/sitemap.xml" ||
  { echo "--site-url did not override sitemap URLs" >&2; exit 1; }
set +e
"$CKDOCS" check --root "$repo_root" --site-url 'https://example.test/?unsafe=1' >"$test_root/bad-url.log" 2>&1
bad_url_status=$?
set -e
[ "$bad_url_status" -eq 2 ] || { echo "unsafe --site-url should be a usage error" >&2; exit 1; }

# The repository's ckdocs.yml is a portal of two sites: ck-git-hosting under
# hosting/ (which also takes over the root) and the ckdocs manual under ckdocs/.
for page in index.html llms.txt sitemap.xml .ckdocs hosting/index.html hosting/index.html.md \
  hosting/operations/01-installation.html hosting/operations/01-installation.html.md \
  hosting/protocol/01-ssh-and-control-v1.html hosting/site-index.html hosting/site-index.html.md \
  ckdocs/index.html ckdocs/index.html.md ckdocs/configuration.html ckdocs/site-index.html; do
  [ -f "$site/$page" ] || { echo "expected $page in the built site" >&2; exit 1; }
done

# Addresses of the former single site still lead to their pages.
grep -qF 'url=../hosting/operations/01-installation.html"' "$site/operations/01-installation.html" ||
  { echo "the former address of a page does not redirect to it" >&2; exit 1; }

grep -qF 'rel="alternate" type="text/markdown" href="index.html.md"' "$site/hosting/index.html" ||
  { echo "home page is missing v2 alternate discovery" >&2; exit 1; }
grep -qF 'rel="describedby" href="../../llms.txt"' "$site/hosting/operations/01-installation.html" ||
  { echo "nested page is missing covering-index discovery" >&2; exit 1; }
grep -qF '.html.md)' "$site/llms.txt" || { echo "llms.txt is missing Markdown page links" >&2; exit 1; }

grep -q '>Home</a>' "$site/hosting/index.html" || { echo "hosting/index.html is missing the Home link" >&2; exit 1; }
for tab in Guides Server CI Reference; do
  grep -q "<summary><span>$tab</span>" "$site/hosting/index.html" || { echo "hosting/index.html is missing the $tab dropdown" >&2; exit 1; }
done
for tab in Guide Diagrams Reference; do
  grep -q "<summary><span>$tab</span>" "$site/ckdocs/index.html" || { echo "ckdocs/index.html is missing the $tab dropdown" >&2; exit 1; }
done

# The site selector leads between both manuals; each carries its own logo.
grep -qF '<a href="../ckdocs/index.html">ckdocs</a>' "$site/hosting/index.html" ||
  { echo "the ck-git-hosting site does not offer the ckdocs manual" >&2; exit 1; }
grep -qF '<a href="../hosting/index.html">ck-git-hosting</a>' "$site/ckdocs/index.html" ||
  { echo "the ckdocs manual does not offer the ck-git-hosting site" >&2; exit 1; }
grep -qF '<img src="../docs/images/ck-git-hosting.svg" alt="">' "$site/hosting/index.html" ||
  { echo "the ck-git-hosting site is missing its logo" >&2; exit 1; }
grep -qF '<img src="images/ckdocs.svg" alt="">' "$site/ckdocs/index.html" ||
  { echo "the ckdocs manual is missing its own logo" >&2; exit 1; }
grep -qF 'href="../../ckdocs/configuration.html' "$site/hosting/operations/07-docs-sites.html" ||
  { echo "a link from one site's page into the other site did not resolve" >&2; exit 1; }

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
  grep -oE '(href|src|srcset)="[^"]+"' "$page" >"$refs_list" || true
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

# Each diagram type has a focused example page with its copyable source.
diagram_count=0
for gallery in "$site"/ckdocs/diagrams/*.html; do
  count=$(grep -o 'class="mermaid"' "$gallery" | wc -l | tr -d ' ')
  [ "$count" -eq 1 ] || { echo "$gallery: expected one rendered diagram, got $count" >&2; exit 1; }
  grep -q 'class="language-markdown"' "$gallery" || { echo "$gallery: missing diagram source" >&2; exit 1; }
  if grep -q 'class="language-mermaid"' "$gallery"; then
    echo "$gallery: Mermaid example fell back to a code block" >&2
    exit 1
  fi
  diagram_count=$((diagram_count + count))
done
[ "$diagram_count" -eq 23 ] || { echo "expected all 23 Mermaid example pages, got $diagram_count" >&2; exit 1; }
for svg in "$site"/ckdocs/_ckdocs-mermaid/*.svg; do
  grep -q '<svg ' "$svg" || { echo "invalid generated SVG: $svg" >&2; exit 1; }
done

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

# ---- front matter: author, date, and another tool's declared keys ----------

tagged="$test_root/tagged"
mkdir -p "$tagged"
cat >"$tagged/ckdocs.yml" <<'EOF'
version: 1
site:
  title: Tagged
front_matter:
  foreign_keys: [format]
EOF
cat >"$tagged/README.md" <<'EOF'
---
author: C. Klukas
date: 2026-08-09
format: pdf
---
# Tagged

Body.
EOF

"$CKDOCS" check --root "$tagged" >"$test_root/tagged-check.log" 2>&1 || {
  echo "ckdocs check should accept a declared foreign front matter key:" >&2
  cat "$test_root/tagged-check.log" >&2
  exit 1
}
"$CKDOCS" build --root "$tagged" --out "$test_root/tagged-site" --strict --quiet
for expected in '<meta name="author" content="C. Klukas">' '<meta name="date" content="2026-08-09">' \
  '<meta name="front-matter:format" content="pdf">' \
  '<p class="byline"><span class="author">C. Klukas</span> · <time datetime="2026-08-09">2026-08-09</time></p>'; do
  grep -qF "$expected" "$test_root/tagged-site/index.html" || {
    echo "the tagged page is missing: $expected" >&2
    exit 1
  }
done

cat >"$tagged/other.md" <<'EOF'
---
permalink: /other/
---
# Other
EOF
set +e
tagged_output=$("$CKDOCS" check --root "$tagged" 2>&1)
tagged_status=$?
set -e
[ "$tagged_status" -eq 1 ] || {
  echo "ckdocs check should fail on an undeclared front matter key, got $tagged_status:" >&2
  echo "$tagged_output" >&2
  exit 1
}
case "$tagged_output" in
  *"other.md: unknown front matter key 'permalink' ignored"*) ;;
  *)
    echo "ckdocs check's report should name the page and the undeclared key:" >&2
    echo "$tagged_output" >&2
    exit 1
    ;;
esac

# ---- Mermaid warnings use the normal build/check exit contract ------------

mermaid_fixture="$test_root/mermaid-errors"
mkdir -p "$mermaid_fixture"
cat >"$mermaid_fixture/README.md" <<'EOF'
# Mermaid errors

```mermaid
flowchart LR
A[Safe] --> B[Output]
```

```mermaid
unknownDiagram
<script>alert(1)</script>
```

```text
flowchart LR
A --> B
```
EOF
set +e
"$CKDOCS" build --root "$mermaid_fixture" --out "$test_root/mermaid-errors-site" >"$test_root/mermaid-errors.log" 2>&1
mermaid_status=$?
set -e
[ "$mermaid_status" -eq 3 ] || { echo "Mermaid warnings should return 3, got $mermaid_status" >&2; exit 1; }
grep -q 'README.md: Mermaid diagram 2: unknown diagram type' "$test_root/mermaid-errors.log"
grep -q 'class="mermaid"' "$test_root/mermaid-errors-site/index.html"
grep -q 'class="language-mermaid"' "$test_root/mermaid-errors-site/index.html"
grep -q '&lt;script&gt;alert(1)&lt;/script&gt;' "$test_root/mermaid-errors-site/index.html"
grep -q 'class="language-text"' "$test_root/mermaid-errors-site/index.html"
set +e
"$CKDOCS" check --root "$mermaid_fixture" >"$test_root/mermaid-check.log" 2>&1
mermaid_status=$?
set -e
[ "$mermaid_status" -eq 1 ] || { echo "Mermaid warnings should fail check, got $mermaid_status" >&2; exit 1; }

# ---- several sites in one build: a portal over two branches ---------------

portal_repo="$test_root/portal"
mkdir -p "$portal_repo/docs"
git -C "$portal_repo" -c init.defaultBranch=main init -q
portal_commit() {
  git -C "$portal_repo" add -A
  git -C "$portal_repo" -c user.email=t@example.invalid -c user.name=Test -c commit.gpgSign=false commit -q -m "$1"
}
printf 'version: 1\nsite:\n  title: Python docs\n' >"$portal_repo/ckdocs.yml"
printf '# Python home\n\nRead the [guide](guide.md).\n' >"$portal_repo/docs/README.md"
printf '# Guide\n\nBody.\n' >"$portal_repo/docs/guide.md"
portal_commit python
git -C "$portal_repo" checkout -q -b cpp
mkdir -p "$portal_repo/cpp-docs"
cat >"$portal_repo/ckdocs.yml" <<'YAML'
version: 1
site:
  title: Portal
sites:
  - id: cpp
    title: C++ edition
    config: cpp-docs/ckdocs.yml
  - id: python
    title: Python edition
    ref: main
    redirect_from_root: true
YAML
printf 'version: 1\nsite:\n  title: C++ docs\nsource: cpp-docs\n' >"$portal_repo/cpp-docs/ckdocs.yml"
printf '# C++ home\n\nSee the [API](api.md).\n' >"$portal_repo/cpp-docs/README.md"
printf '# API\n' >"$portal_repo/cpp-docs/api.md"
portal_commit cpp

portal_site="$test_root/portal-site"
mkdir "$test_root/no-external-tar"
printf '#!/bin/sh\nexit 99\n' >"$test_root/no-external-tar/tar"
chmod +x "$test_root/no-external-tar/tar"
PATH="$test_root/no-external-tar:$PATH" "$CKDOCS" build --root "$portal_repo" --out "$portal_site" --strict >"$test_root/portal.log" 2>&1 || {
  echo "ckdocs build of a portal failed:" >&2
  cat "$test_root/portal.log" >&2
  exit 1
}
grep -q 'Built 5 page(s) in 2 site(s), 0 asset(s), 1 redirect(s)' "$test_root/portal.log" || {
  echo "unexpected portal build summary:" >&2
  cat "$test_root/portal.log" >&2
  exit 1
}
for page in index.html .ckdocs cpp/index.html cpp/api.html python/index.html python/guide.html guide.html; do
  [ -f "$portal_site/$page" ] || { echo "expected $page in the portal" >&2; exit 1; }
done
grep -qF '<a href="python/index.html">Python edition</a>' "$portal_site/index.html" ||
  { echo "the welcome page is missing the Python card" >&2; exit 1; }
grep -qF '<summary>Python edition</summary>' "$portal_site/python/guide.html" ||
  { echo "a Python page is missing its site selector" >&2; exit 1; }
grep -qF '<a href="../cpp/index.html">C++ edition</a>' "$portal_site/python/guide.html" ||
  { echo "a Python page's selector does not link the C++ site" >&2; exit 1; }
grep -qF 'url=python/guide.html' "$portal_site/guide.html" ||
  { echo "the old root URL does not redirect into the Python site" >&2; exit 1; }

# A tree without .git (a CI checkout, say) needs --ref-tree for the ref.
portal_ci="$test_root/portal-ci"
portal_main="$test_root/portal-main"
mkdir -p "$portal_ci" "$portal_main"
git -C "$portal_repo" archive --format=tar --output="$test_root/portal-cpp.tar" cpp
tar -xf "$test_root/portal-cpp.tar" -C "$portal_ci"
git -C "$portal_repo" archive --format=tar --output="$test_root/portal-main.tar" main
tar -xf "$test_root/portal-main.tar" -C "$portal_main"
set +e
portal_output=$("$CKDOCS" check --root "$portal_ci" 2>&1)
portal_status=$?
set -e
[ "$portal_status" -eq 1 ] || { echo "a portal ref with no repository should fail, got $portal_status" >&2; exit 1; }
case "$portal_output" in
  *"site 'python': ref 'main' is not available"*"--ref-tree main=DIR"*) ;;
  *) echo "the missing-ref error should name the site and --ref-tree:" >&2; echo "$portal_output" >&2; exit 1 ;;
esac
(cd "$test_root" && "$CKDOCS" check --root portal-ci --ref-tree main=portal-main --quiet) ||
  { echo "ckdocs check with a relative --ref-tree failed" >&2; exit 1; }
"$CKDOCS" build --root "$portal_ci" --ref-tree "main=$portal_main" --out "$test_root/portal-ci-site" --strict --quiet
cmp -s "$portal_site/python/guide.html" "$test_root/portal-ci-site/python/guide.html" ||
  { echo "--ref-tree should build the same page as the extracted ref" >&2; exit 1; }

set +e
"$CKDOCS" check --root "$fixture" --ref-tree main=. >/dev/null 2>&1
ref_tree_status=$?
"$CKDOCS" check --root "$portal_repo" --source docs >/dev/null 2>&1
source_status=$?
set -e
[ "$ref_tree_status" -eq 2 ] || { echo "--ref-tree without sites should be a usage error, got $ref_tree_status" >&2; exit 1; }
[ "$source_status" -eq 2 ] || { echo "--source with sites should be a usage error, got $source_status" >&2; exit 1; }

# ---- serve: a loopback preview server --------------------------------------

serve_log="$test_root/serve.log"
"$CKDOCS" serve --root "$repo_root" --port 0 --quiet >"$serve_log" 2>&1 &
serve_pid=$!

await_serve "$serve_log"

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
check_serve "/hosting/operations/07-docs-sites.html" "HTTP/1.1 200 OK" "text/html"
check_serve "/hosting/operations/07-docs-sites.html.md" "HTTP/1.1 200 OK" "text/plain"
grep -qF "#" "$test_root/serve-body" || { echo "Markdown response is missing article content" >&2; exit 1; }
if grep -qF "<!doctype html>" "$test_root/serve-body"; then echo "Markdown URL returned the HTML shell" >&2; exit 1; fi
check_serve "/llms.txt" "HTTP/1.1 200 OK" "text/plain"
check_serve "/hosting/operations/08-web-dashboard.html" "HTTP/1.1 200 OK" "text/html"
check_serve "/hosting/images/web-files.png" "HTTP/1.1 200 OK" "image/png"
diagram_asset=$(basename "$(find "$site/ckdocs/_ckdocs-mermaid" -name '*.svg' | head -n 1)")
check_serve "/ckdocs/_ckdocs-mermaid/$diagram_asset" "HTTP/1.1 200 OK" "image/svg+xml"
check_serve "/nope.html" "HTTP/1.1 404 Not Found"

stop_serve || exit 1
echo "docs_site serve check passed"

# One unusable file does not stop a build: an oversized page is left out with
# a warning, and links are reported as page:line, once per target.
findings_fixture="$test_root/findings"
mkdir -p "$findings_fixture/docs"
printf '# Home\n\n[guide](guide.md)\n' > "$findings_fixture/docs/index.md"
printf -- '---\ntitle: Guide\n---\n# Guide\n[a](nope.md)\n\n[b](nope.md) [big](big.md)\n' > "$findings_fixture/docs/guide.md"
{ printf '# Big\n\n'; head -c 600000 /dev/zero | tr '\0' x; printf '\n'; } > "$findings_fixture/docs/big.md"
set +e
"$CKDOCS" build --root "$findings_fixture" --out "$test_root/findings-site" >"$test_root/findings.log" 2>&1
findings_status=$?
set -e
[ "$findings_status" -eq 3 ] || { cat "$test_root/findings.log" >&2; echo "findings should return 3, got $findings_status" >&2; exit 1; }
grep -qx "warning: docs/big.md: exceeds the 512 KiB page limit; left out of the site (split it, or list it under exclude in ckdocs.yml)" \
  "$test_root/findings.log" || { cat "$test_root/findings.log" >&2; echo "the oversized page was not reported as left out" >&2; exit 1; }
grep -qx "warning: docs/guide.md:5: link target 'nope.md' is not a page of this site (2 links; also line 7)" \
  "$test_root/findings.log" || { cat "$test_root/findings.log" >&2; echo "repeated broken links were not grouped with lines" >&2; exit 1; }
grep -qx "warning: docs/guide.md:7: link target 'big.md' was left out of the site: it exceeds the 512 KiB page limit" \
  "$test_root/findings.log" || { cat "$test_root/findings.log" >&2; echo "a link to the left-out page did not say why" >&2; exit 1; }
[ -f "$test_root/findings-site/guide.html" ] && [ ! -e "$test_root/findings-site/big.html" ] ||
  { echo "the site was not built without the oversized page" >&2; exit 1; }
set +e
"$CKDOCS" check --root "$findings_fixture" >"$test_root/findings-check.log" 2>&1
findings_status=$?
set -e
[ "$findings_status" -eq 1 ] || { echo "check should fail on a left-out page, got $findings_status" >&2; exit 1; }

# serve rebuilds on change: an edited page, a new page, and a broken
# configuration (which keeps the last good site) while it runs.
watch_log="$test_root/watch.log"
"$CKDOCS" serve --root "$findings_fixture" --port 0 >"$watch_log" 2>&1 &
serve_pid=$!
await_serve "$watch_log"
wait_served() {  # wait_served PATH TEXT: until the served page contains TEXT
  attempt=0
  until curl --max-time 4 --silent "http://127.0.0.1:$serve_port$1" | grep -q "$2"; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || { cat "$watch_log" >&2; echo "serve did not rebuild $1 with '$2'" >&2; exit 1; }
    sleep .1
  done
}
wait_served /guide.html 'Guide'
# While watching, served pages carry the live-update script (the built files
# do not), and the event stream names exactly the outputs a rebuild changed.
curl --max-time 4 --silent "http://127.0.0.1:$serve_port/guide.html" | grep -q '<script data-ckdocs-live>' ||
  { echo "a served page lacks the live-update script" >&2; exit 1; }
! grep -q 'data-ckdocs-live' "$test_root/findings-site/guide.html" || { echo "a built page carries the live-update script" >&2; exit 1; }
events="$test_root/live-events"
curl --no-buffer --max-time 30 --silent "http://127.0.0.1:$serve_port/__ckdocs/events" >"$events" &
events_pid=$!
await_event() {  # await_event PATTERN: until the event stream contains PATTERN
  attempt=0
  until grep -q "$1" "$events"; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || { cat "$events" >&2; echo "no live event matching '$1'" >&2; exit 1; }
    sleep .1
  done
}
await_event '^event: hello$'
printf '# Guide\n\nEdited-while-serving.\n' > "$findings_fixture/docs/guide.md"
wait_served /guide.html 'Edited-while-serving'
await_event '^data: {"build":2,"changed":\[.*"/guide.html"'
! grep -q '"/index.html"' "$events" || { cat "$events" >&2; echo "an unchanged page was listed as changed" >&2; exit 1; }
printf '# Fresh\n\nA new page.\n' > "$findings_fixture/docs/fresh.md"
wait_served /fresh.html 'A new page'
grep -q '^Rebuilt ' "$watch_log" || { cat "$watch_log" >&2; echo "serve did not report its rebuild" >&2; exit 1; }
printf 'version: 2\n' > "$findings_fixture/ckdocs.yml"
attempt=0
until grep -q 'still serving the last build' "$watch_log"; do
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { cat "$watch_log" >&2; echo "a failed rebuild was not reported" >&2; exit 1; }
  sleep .1
done
await_event '^event: failure$'
wait_served /fresh.html 'A new page'
kill "$events_pid" 2>/dev/null || true
wait "$events_pid" 2>/dev/null || true
stop_serve || exit 1
rm -f "$findings_fixture/ckdocs.yml"
echo "docs_site watch check passed"

echo "docs_site integration test passed"
