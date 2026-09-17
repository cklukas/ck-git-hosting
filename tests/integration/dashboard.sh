#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

set -eu
test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
[ -d "$test_root_parent" ] || { echo "Approved temporary volume is unavailable" >&2; exit 1; }
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac

test_root=$(mktemp -d "$test_root_parent/ckd.XXXXXX")
server_pid=''
slow_pids=''
cleanup() {
  for pid in $slow_pids; do kill "$pid" 2>/dev/null || true; done
  for pid in $slow_pids; do wait "$pid" 2>/dev/null || true; done
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$test_root/tmp"
TMPDIR="$test_root/tmp"
export TMPDIR
GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_NOSYSTEM

"$CKGIT_ADMIN" create alpha --repo-root "$test_root" >/dev/null
"$CKGIT_ADMIN" create empty --repo-root "$test_root" >/dev/null
mkdir "$test_root/state" "$test_root/hooks" "$test_root/bin"
chmod 0700 "$test_root/state"
cp "$CKGIT_POST_RECEIVE" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
git init -q --initial-branch=main "$test_root/work"
git -C "$test_root/work" config user.email tests@example.test
git -C "$test_root/work" config user.name 'Dashboard & Tests'
mkdir "$test_root/work/docs" "$test_root/work/docs/nested" \
  "$test_root/work/src" "$test_root/work/src/internal" "$test_root/work/configuration"
cat > "$test_root/work/README.md" <<'EOF'
# Dashboard fixture

Rendered **README**. <script>readme_attack()</script>

![Diagram](docs/diagram.svg)

[Read the guide](docs/guide.md)
EOF
printf '# Directory README\n\n[Guide](guide.md)\n' > "$test_root/work/docs/README.md"
printf '# A guide\n\nSecond line.\n' > "$test_root/work/docs/guide.md"
printf '# Nested guide\n\nA fully expandable sibling tree.\n' > "$test_root/work/docs/nested/guide.md"
printf 'int engine() { return 42; }\n' > "$test_root/work/src/internal/engine.cpp"
printf 'enabled=true\n' > "$test_root/work/configuration/settings.ini"
printf 'Archive icon fixture\n' > "$test_root/work/release.zip"
printf 'first line\n<script>file_attack()</script>\n' > "$test_root/work/docs/Grüße space:?.txt"
printf '%s' '<svg xmlns="http://www.w3.org/2000/svg" onload="svg_attack()"><script>svg_attack()</script><foreignObject>active</foreignObject></svg>' > "$test_root/work/docs/diagram.svg"
cp "$test_root/work/docs/diagram.svg" "$test_root/work/image.txt"
printf '\000\001binary\377' > "$test_root/work/unknown.bin"
dd if=/dev/zero bs=1048576 count=4 2>/dev/null | tr '\000' ' ' > "$test_root/work/large.svg"
printf ' ' >> "$test_root/work/large.svg"
ln -s docs/diagram.svg "$test_root/work/link.svg"
ln -s docs "$test_root/work/docs-link"
git -C "$test_root/work" add .
GIT_AUTHOR_DATE=2024-02-29T12:00:00+0000 GIT_COMMITTER_DATE=2024-02-29T12:00:00+0000 \
  git -C "$test_root/work" commit -q -m 'Initial <script>commit_attack()</script>'
first=$(git -C "$test_root/work" rev-parse HEAD)
git -C "$test_root/work" branch feature/nested
git -C "$test_root/work" tag v1
git -C "$test_root/work" tag main
number=0
while [ "$number" -lt 55 ]; do
  minute=$(printf '%02d' "$number")
  GIT_AUTHOR_DATE="2026-09-04T12:$minute:00+0000" GIT_COMMITTER_DATE="2026-09-04T12:$minute:00+0000" \
    git -C "$test_root/work" commit -q --allow-empty -m "History $minute"
  number=$((number + 1))
done
head=$(git -C "$test_root/work" rev-parse HEAD)
git -C "$test_root/work" remote add local "$test_root/alpha.git"
git -C "$test_root/work" push -q local --all
git -C "$test_root/work" push -q local --tags
git --git-dir "$test_root/alpha.git" config core.hooksPath "$test_root/hooks"

# Only the daemon uses this wrapper, so requests to indexed pages must leave
# its counter unchanged once the initial index is ready.
git_binary=$(command -v git)
: > "$test_root/git-calls"
: > "$test_root/git-arguments"
cat > "$test_root/bin/git" <<EOF
#!/bin/sh
printf 'git\n' >> "$test_root/git-calls"
printf '%s\n' "\$*" >> "$test_root/git-arguments"
exec "$git_binary" "\$@"
EOF
chmod 0755 "$test_root/bin/git"
PATH="$test_root/bin:$PATH" "$CKGIT_HOSTINGD" --repo-root "$test_root" \
  --state-root "$test_root/state" --control-socket "$test_root/control.sock" \
  --hook-directory "$test_root/hooks" --http-port 0 --ssh-clone-target ckgit@fixture \
  > "$test_root/server.log" 2>&1 &
server_pid=$!
attempt=0
port=''
while [ -z "$port" ]; do
  if ! kill -0 "$server_pid" 2>/dev/null; then cat "$test_root/server.log" >&2; exit 1; fi
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$test_root/server.log")
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { echo 'Dashboard listener never became ready' >&2; exit 1; }
  [ -n "$port" ] || sleep .1
done
base_url="http://127.0.0.1:$port"
get() {
  curl --path-as-is --max-time 4 --silent --show-error -D "$test_root/headers" \
    -o "$test_root/body" "$base_url$1"
  grep -q '^HTTP/1.1 200 OK' "$test_root/headers"
  grep -q '^X-Content-Type-Options: nosniff' "$test_root/headers"
}
page() {
  get "$1"
  grep -q '^Content-Type: text/html' "$test_root/headers"
  if grep -q '<script' "$test_root/body"; then echo "Unescaped repository markup: $1" >&2; exit 1; fi
}
reject() {
  if grep -q "$1" "$2"; then echo "Unexpected content '$1' in $2" >&2; return 1; fi
}
attempt=0
while :; do
  page /project/alpha
  grep -q 'History 54' "$test_root/body" && break
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { echo 'Startup index did not complete' >&2; exit 1; }
  sleep .1
done
attempt=0
while :; do
  page /project/empty
  grep -q 'No commits published yet' "$test_root/body" && break
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { echo 'Empty project index did not complete' >&2; exit 1; }
  sleep .1
done
cp "$test_root/git-calls" "$test_root/calls-before"
page /
grep -q 'Last commit' "$test_root/body"
grep -q 'History 54' "$test_root/body"
reject 'img-src' "$test_root/headers"
page /project/alpha
grep -q 'Rendered <strong>README</strong>' "$test_root/body"
grep -q '&lt;script&gt;readme_attack()' "$test_root/body"
grep -q "img-src 'self'" "$test_root/headers"
page /by-name
cmp "$test_root/calls-before" "$test_root/git-calls"

# Clone instructions use the administrator's SSH target for both populated and
# empty projects; the browser's local tunnel address and Host header are not
# destinations from which a repository can be cloned.
for project in alpha empty; do
  page "/project/$project"
  grep -q "<code>ckgit clone $project</code>" "$test_root/body"
  grep -q "<code>git clone ckgit@fixture:$project.git</code>" "$test_root/body"
  [ "$(grep -o 'class="clone-info"' "$test_root/body" | wc -l | tr -d ' ')" -eq 1 ]
  reject 'git clone .*127\.0\.0\.1' "$test_root/body"
done
curl --max-time 4 --silent --show-error -H 'Host: attacker.invalid:5000' \
  -D "$test_root/headers" -o "$test_root/body" "$base_url/project/alpha"
grep -q '^HTTP/1.1 200 OK' "$test_root/headers"
grep -q '<code>git clone ckgit@fixture:alpha.git</code>' "$test_root/body"
reject 'attacker.invalid' "$test_root/body"

page "/project/alpha/tree/$head:"
grep -q '<details' "$test_root/body"
grep -q 'tree-pane' "$test_root/body"
page "/project/alpha/tree/$head:docs"
grep -q 'Directory README' "$test_root/body"
grep -q "/project/alpha/blob/$head:docs/guide.md" "$test_root/body"
page /project/alpha/tree/main:
grep -q 'branch and a tag share this name' "$test_root/body"
page /project/alpha/tree/heads/feature/nested:
page /project/alpha/tree/tags/main:
grep -q '<details class="tree-folder"' "$test_root/body"
grep -q '<summary class="tree-row' "$test_root/body"
grep -q '/project/alpha/blob/tags/main:src/internal/engine.cpp' "$test_root/body"
grep -q '/project/alpha/blob/tags/main:docs/nested/guide.md' "$test_root/body"
reject 'Open folder to load contents' "$test_root/body"
for category in folder code markdown config image archive file link; do
  grep -q "tree-icon icon-$category" "$test_root/body"
done
# Browsing a deeply nested file needs one recursive listing plus the file's
# own directory lookup; the number of ancestor or sibling folders adds no Git
# calls. Its entire sidebar is already present for native disclosure toggles.
tree_calls_before=$(grep -c ' ls-tree ' "$test_root/git-arguments" || true)
page /project/alpha/blob/tags/main:docs/nested/guide.md
tree_calls_after=$(grep -c ' ls-tree ' "$test_root/git-arguments" || true)
[ "$((tree_calls_after - tree_calls_before))" -eq 2 ] || {
  echo 'File navigation should use one recursive tree read and one file lookup' >&2
  exit 1
}
sed -n 's/.*<aside class="tree-pane"[^>]*>\(.*\)<\/aside>.*/\1/p' "$test_root/body" >"$test_root/navigation.html"
[ "$(grep -o 'aria-current="page"' "$test_root/navigation.html" | wc -l | tr -d ' ')" -eq 1 ]
grep -q '/project/alpha/blob/tags/main:src/internal/engine.cpp' "$test_root/navigation.html"
grep -q "<a[^>]*href=\"/project/alpha/blob/tags/main:docs-link\"" "$test_root/navigation.html"
reject 'docs-link/' "$test_root/navigation.html"
grep -q "/project/alpha/raw/$first:docs/nested/guide.md" "$test_root/body"
page "/project/alpha/blob/$head:docs/Gr%C3%BC%C3%9Fe%20space%3A%3F.txt"
grep -q 'id="L1"' "$test_root/body"
grep -q 'id="L2"' "$test_root/body"
grep -q '&lt;script&gt;file_attack()' "$test_root/body"
reject 'img-src' "$test_root/headers"
page "/project/alpha/blob/$head:docs/diagram.svg"
grep -q "<img src=\"/project/alpha/raw/$head:docs/diagram.svg\"" "$test_root/body"
reject 'svg_attack\|foreignObject' "$test_root/body"
grep -q "img-src 'self'" "$test_root/headers"
page "/project/alpha/blob/$head:large.svg"
grep -q '4 MiB' "$test_root/body"
reject '<img ' "$test_root/body"
reject 'img-src' "$test_root/headers"
page "/project/alpha/blob/$head:link.svg"
grep -q 'target is never followed' "$test_root/body"
reject '<img ' "$test_root/body"
page "/project/alpha/blob/$head:unknown.bin"
grep -q 'Binary file' "$test_root/body"

raw_path="/project/alpha/raw/$head:docs/diagram.svg"
get "$raw_path"
cmp "$test_root/work/docs/diagram.svg" "$test_root/body"
grep -q '^Content-Type: image/svg+xml' "$test_root/headers"
grep -q '^Content-Disposition: attachment;' "$test_root/headers"
grep -q "Content-Security-Policy: default-src 'none'.*sandbox" "$test_root/headers"
grep -q 'max-age=31536000, immutable' "$test_root/headers"
raw_length=$(wc -c < "$test_root/work/docs/diagram.svg" | tr -d ' ')
curl --max-time 4 --silent --show-error --head "$base_url$raw_path" > "$test_root/head.headers"
grep -q '^HTTP/1.1 200 OK' "$test_root/head.headers"
grep -q "^Content-Length: $raw_length" "$test_root/head.headers"
get "/project/alpha/raw/$head:image.txt"
grep -q '^Content-Type: text/plain; charset=utf-8' "$test_root/headers"
get "/project/alpha/raw/$head:unknown.bin"
grep -q '^Content-Type: application/octet-stream' "$test_root/headers"

page /project/alpha/commits/heads/main
[ "$(grep -o 'class="graph-cell"' "$test_root/body" | wc -l | tr -d ' ')" -eq 50 ]
older=$(grep -o 'href="[^"]*/before/[0-9a-f]*"' "$test_root/body" | head -1 | sed 's/^href="//;s/"$//')
[ -n "$older" ]
page "$older"
[ "$(grep -o 'class="graph-cell"' "$test_root/body" | wc -l | tr -d ' ')" -eq 6 ]
grep -q 'Initial &lt;script&gt;' "$test_root/body"
page /project/alpha/graph/heads/main
grep -q '<svg class="graph graph-lines"' "$test_root/body"
grep -q 'preserveAspectRatio="none"' "$test_root/body"
grep -q '<svg class="graph graph-node"' "$test_root/body"
[ "$(grep -o 'class="graph graph-lines"' "$test_root/body" | wc -l | tr -d ' ')" -eq 50 ]
[ "$(grep -o 'class="graph graph-node"' "$test_root/body" | wc -l | tr -d ' ')" -eq 50 ]
page "/project/alpha/commit/$first"
grep -q 'Root commit' "$test_root/body"
grep -q 'Changed files' "$test_root/body"
grep -q '&lt;script&gt;commit_attack()' "$test_root/body"
page /project/alpha/calendar/heads/main/2024/02
grep -q 'February 2024' "$test_root/body"
grep -q '2024-02-29' "$test_root/body"
grep -q 'year-strip' "$test_root/body"
page /project/alpha/calendar/heads/main
page /project/alpha/day/heads/main/2024-02-29
grep -q 'Browse the tree as of this day' "$test_root/body"
grep -q "/tree/$first:" "$test_root/body"
page /project/alpha/day/heads/main/2024-02-28
grep -q 'No tree existed' "$test_root/body"

for invalid in 'tree/main:../x' 'tree/main:a%2Fb' 'tree/main:a%2fb' 'tree/main:a%00b' \
    'tree/main:%C0%AF' 'tree/-main:x' 'tree/main:a:b' 'raw/main:README.md' \
    'commit/abc123' 'day/main/2023-02-29'; do
  status=$(curl --path-as-is --max-time 4 --silent --show-error -o "$test_root/body" \
    -w '%{http_code}' "$base_url/project/alpha/$invalid")
  case "$status" in 400|404) ;; *) echo "Noncanonical route accepted: $invalid ($status)" >&2; exit 1 ;; esac
done

# Three partial headers occupy three workers. The fourth must serve the table
# before the incomplete clients close, demonstrating independent progress.
for number in 1 2 3; do
  { printf 'GET / HTTP/1.1\r\nHost:'; sleep 3; } | \
    nc -w 4 127.0.0.1 "$port" > "$test_root/slow-$number.out" &
  slow_pids="$slow_pids $!"
done
curl --max-time 2 --silent --show-error "$base_url/" > "$test_root/concurrent.body"
grep -q '<h1>Projects</h1>' "$test_root/concurrent.body"
for pid in $slow_pids; do wait "$pid" 2>/dev/null || true; done
slow_pids=''

GIT_AUTHOR_DATE=2026-09-05T12:00:00+0000 GIT_COMMITTER_DATE=2026-09-05T12:00:00+0000 \
  git -C "$test_root/work" commit -q --allow-empty -m 'Push refresh visible'
CKGIT_STATE_ROOT="$test_root/state" CKGIT_CLIENT_ID=integration CKGIT_PROJECT_NAME=alpha \
  CKGIT_CONTROL_SOCKET="$test_root/control.sock" \
  git -C "$test_root/work" push -q local HEAD:refs/heads/main
attempt=0
while :; do
  page /
  grep -q 'Push refresh visible' "$test_root/body" && break
  attempt=$((attempt + 1))
  [ "$attempt" -lt 20 ] || { echo 'Push hook did not refresh the table promptly' >&2; exit 1; }
  sleep .1
done
page /project/alpha
grep -q 'Push refresh visible' "$test_root/body"
echo 'dashboard integration test passed'
