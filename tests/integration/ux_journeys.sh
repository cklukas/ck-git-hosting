#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

# End-user regression journeys for review findings C1-C6 and W1-W5. All Git,
# SSH, registrations, configuration, and HTTP requests belong to this fixture.
set -eu
test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
[ -d "$test_root_parent" ] || { echo 'Approved temporary volume is unavailable' >&2; exit 1; }
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/cku.XXXXXX")
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$test_root/tmp" "$test_root/test-bin" "$test_root/repos" "$test_root/state" \
  "$test_root/hooks" "$test_root/first" "$test_root/second" "$test_root/offline"
chmod 0700 "$test_root/state"
TMPDIR="$test_root/tmp"
XDG_CONFIG_HOME="$test_root/offline"
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_NOSYSTEM=1
GIT_TERMINAL_PROMPT=0
CKGIT_TEST_CLIENT_ID=device-one
TEST_ROOT="$test_root"
export TMPDIR XDG_CONFIG_HOME GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM GIT_TERMINAL_PROMPT \
  CKGIT_TEST_CLIENT_ID TEST_ROOT
unset GIT_SSH GIT_SSH_COMMAND

fail() { echo "UX journeys: $*" >&2; exit 1; }
contains() {
  case "$1" in *"$2"*) ;; *) fail "expected '$2' in: $1" ;; esac
}
lacks() {
  case "$1" in *"$2"*) fail "unexpected '$2' in: $1" ;; *) ;; esac
}
capture() {
  set +e
  "$@" >"$test_root/command.out" 2>"$test_root/command.err"
  command_status=$?
  set -e
  command_output=$(cat "$test_root/command.out" "$test_root/command.err")
}
succeeded() {
  [ "$command_status" -eq 0 ] || fail "command returned $command_status: $command_output"
}
attention() {
  case "$command_status" in 0|3) ;; *) fail "unexpected command status $command_status: $command_output" ;; esac
}
new_repo() {
  git init -q --initial-branch=main "$1"
  git -C "$1" config user.email tests@example.test
  git -C "$1" config user.name 'User Journey Tests'
}
commit() {
  GIT_AUTHOR_DATE=2024-02-29T12:00:00+0000 GIT_COMMITTER_DATE=2024-02-29T12:00:00+0000 \
    git -C "$1" -c commit.gpgSign=false commit -q -m "$2"
}
same_ref() {
  [ "$(git -C "$1" rev-parse "$2")" = "$(git --git-dir "$test_root/repos/hosted.git" rev-parse "$2")" ] ||
    fail "server ref differs from $1: $2"
}
absent_ref() {
  if git --git-dir "$test_root/repos/hosted.git" show-ref --verify --quiet "$1"; then
    fail "a preview, exclusion, or secondary checkout uploaded $1"
  fi
}
encode_checkout_path() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }

# Help is successful, discoverable, and offline, even with a broken explicit
# config. The SSH trap proves that help never attempts a connection.
cat >"$test_root/test-bin/ssh" <<'SH'
#!/bin/sh
printf 'unexpected SSH\n' >>"$TEST_ROOT/ssh-calls"
exit 91
SH
chmod 0700 "$test_root/test-bin/ssh"
PATH="$test_root/test-bin:$PATH"
export PATH
: >"$test_root/ssh-calls"
help_ok() {
  capture "$CKGIT" "$@"
  succeeded
  [ -s "$test_root/command.out" ] || fail 'help must print to stdout'
  [ ! -s "$test_root/command.err" ] || fail "help must not print errors: $command_output"
  contains "$command_output" ckgit
}
help_ok --help
help_ok -h
help_ok help
for command in scan status clone create publish register sync web; do
  help_ok "$command" --help
  help_ok "$command" -h
  help_ok help "$command"
  help_ok "$command" --config "$test_root/does-not-exist.ini" --help
done
for nested in list set-canonical migrate; do
  help_ok checkout "$nested" --help
  help_ok checkout "$nested" -h
  help_ok help checkout "$nested"
done
help_ok checkout --help
help_ok checkout -h
help_ok config --help
help_ok config -h
help_ok config show --help
help_ok config show -h
help_ok help config show
help_ok --version
contains "$command_output" 'ckgit '
[ ! -s "$test_root/ssh-calls" ] || fail 'offline help used SSH'
capture "$CKGIT" publish --not-an-option
[ "$command_status" -eq 2 ] || fail 'unknown option must return usage error 2'
contains "$command_output" --not-an-option
contains "$command_output" 'ckgit help publish'
capture "$CKGIT" publish --name
[ "$command_status" -eq 2 ] || fail 'missing option value must return usage error 2'
contains "$command_output" --name
contains "$command_output" 'ckgit help publish'

client_config="$test_root/first/client.ini"
second_config="$test_root/second/client.ini"
write_config() {
  printf '%s\n' schema_version=1 "client_id=$2" "display_name=$3" \
    server=ckgit@fixture remote_name=ckgit public_path_mode=basename >"$1"
}
write_config "$client_config" device-one 'First Device'
write_config "$second_config" device-two 'Second Device'
mkdir "$test_root/offline/ck-git-hosting"
cp "$client_config" "$test_root/offline/ck-git-hosting/client.ini"
capture "$CKGIT" config show
succeeded
contains "$command_output" device-one
contains "$command_output" ckgit@fixture
[ ! -s "$test_root/ssh-calls" ] || fail 'config show attempted a connection'

cp "$CKGIT_POST_RECEIVE" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
"$CKGIT_ADMIN" create empty --repo-root "$test_root/repos" >/dev/null
"$CKGIT_HOSTINGD" --repo-root "$test_root/repos" --control-socket "$test_root/control.sock" \
  --state-root "$test_root/state" --hook-directory "$test_root/hooks" --http-port 0 \
  >"$test_root/server.log" 2>&1 &
server_pid=$!
attempt=0
port=''
while [ -z "$port" ] || [ ! -S "$test_root/control.sock" ]; do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server.log" >&2
    fail 'fixture daemon exited before becoming ready'
  fi
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$test_root/server.log")
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || fail 'fixture listeners did not become ready'
  [ -n "$port" ] && [ -S "$test_root/control.sock" ] || sleep .1
done
cat >"$test_root/test-bin/ssh" <<'SH'
#!/bin/sh
for argument do request=$argument; done
printf '%s\n' "$request" >>"$TEST_ROOT/ssh-calls"
SSH_ORIGINAL_COMMAND="$request" exec "$CK_GIT_SHELL" --client-id "$CKGIT_TEST_CLIENT_ID" \
  --repo-root "$TEST_ROOT/repos" --control-socket "$TEST_ROOT/control.sock" --state-root "$TEST_ROOT/state"
SH
chmod 0700 "$test_root/test-bin/ssh"
client() { "$CKGIT" "$@" --config "$client_config"; }
second() { CKGIT_TEST_CLIENT_ID=device-two "$CKGIT" "$@" --config "$second_config"; }

# A differently named source folder deliberately exercises existing identity.
# Its initial publish omits refs, then default sync expands them using only its
# private inventory: this configuration intentionally has no scan_root entries.
source="$test_root/source-folder"
new_repo "$source"
mkdir "$source/docs"
cat >"$source/README.md" <<'EOF'
# Welcome

<!-- This maintenance comment must remain invisible in rendered documentation. -->

[Read the guide](docs/guide.md#install-now)

[Documentation directory](docs)

![Diagram](docs/diagram.svg)
EOF
cat >"$source/docs/guide.md" <<'EOF'
# The guide

## Install now

The **rendered guide** is readable.

[Welcome](../README.md#welcome)

<script>never_run_this()</script>
EOF
printf '# Documentation\n\n[Guide](guide.md#install-now)\n' >"$source/docs/README.md"
printf '<svg xmlns="http://www.w3.org/2000/svg"><text>Safe image</text></svg>\n' >"$source/docs/diagram.svg"
printf 'one\ntwo\n' >"$source/plain.txt"
git -C "$source" add .
commit "$source" 'Initial documentation'
initial=$(git -C "$source" rev-parse HEAD)
git -C "$source" branch feature/shared
git -C "$source" tag -a release/shared -m 'Release on the same commit as the feature branch'
for number in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15; do
  git -C "$source" branch "topic/$number"
done
printf '\nUNCOMMITTED_SENTINEL\n' >>"$source/README.md"
printf 'UNTRACKED_SENTINEL\n' >"$source/untracked.txt"
capture client publish --name hosted --branch main --no-tags --verbose --dry-run "$source"
succeeded
contains "$command_output" hosted
contains "$command_output" ckgit@fixture
contains "$command_output" "$source"
contains "$command_output" 'source folder:'
contains "$command_output" 'default branch:'
contains "$command_output" 'new on server:'
contains "$command_output" 'left out by --branch/--no-tags:'
contains "$command_output" refs/heads/main
contains "$command_output" refs/heads/feature/shared
contains "$command_output" refs/tags/release/shared
contains "$command_output" refs/heads/topic/15
contains "$command_output" main
contains "$command_output" excluded
contains "$command_output" uncommitted
contains "$command_output" 'main checkout on this device:'
contains "$command_output" 'server checkout report:'
[ ! -e "$test_root/repos/hosted.git" ] || fail 'publish preview created the project'
[ -z "$(git -C "$source" remote)" ] || fail 'publish preview added a remote'
capture client publish --name hosted --branch main --no-tags --yes "$source"
succeeded
same_ref "$source" refs/heads/main
absent_ref refs/heads/feature/shared
absent_ref refs/tags/release/shared
checkouts=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 checkouts' "$CK_GIT_SHELL" --client-id device-one \
  --repo-root "$test_root/repos" --control-socket "$test_root/control.sock")
contains "$checkouts" "hosted $(encode_checkout_path source-folder)"
lacks "$checkouts" "$test_root"
contains "$(cat "$test_root/first/canonical.ini")" "hosted=$source"
[ "$(find "$test_root/first/canonical.ini" -prune -type f -perm 0600 -print)" = "$test_root/first/canonical.ini" ] ||
  fail 'private management must store absolute paths in a mode-0600 file'

capture client publish --dry-run --verbose "$source"
succeeded
contains "$command_output" hosted
contains "$command_output" refs/heads/topic/15
capture client publish --name wrong-project --dry-run "$source"
[ "$command_status" -eq 2 ] || fail "conflicting existing identity returned $command_status: $command_output"
contains "$command_output" hosted
contains "$command_output" wrong-project
[ ! -e "$test_root/repos/wrong-project.git" ] || fail 'name conflict created a second project'

capture client sync --dry-run --verbose
succeeded
contains "$command_output" hosted
contains "$command_output" 'First Device'
contains "$command_output" ckgit@fixture
contains "$command_output" "$source"
contains "$command_output" refs/heads/feature/shared
contains "$command_output" refs/heads/topic/15
contains "$command_output" refs/tags/release/shared
contains "$command_output" uncommitted
absent_ref refs/heads/feature/shared
absent_ref refs/tags/release/shared
capture client sync
succeeded
same_ref "$source" refs/heads/feature/shared
same_ref "$source" refs/heads/topic/15
same_ref "$source" refs/tags/release/shared
published_readme=$(git --git-dir "$test_root/repos/hosted.git" show main:README.md)
lacks "$published_readme" UNCOMMITTED_SENTINEL
if git --git-dir "$test_root/repos/hosted.git" cat-file -e main:untracked.txt 2>/dev/null; then
  fail 'sync uploaded untracked working content'
fi
capture client status
attention
contains "$command_output" hosted
contains "$command_output" "$source"
contains "$command_output" 'First Device'
capture client checkout list
succeeded
contains "$command_output" hosted
contains "$command_output" "$source"

# Clone participates in default sync even with basename privacy and no roots.
# A second clone must neither replace the first main checkout nor upload its
# additional refs without an explicit choice by the user.
mkdir "$test_root/clones"
cloned="$test_root/clones/custom-destination"
duplicate="$test_root/clones/second-copy"
capture second clone hosted "$cloned"
succeeded
contains "$command_output" "$cloned"
contains "$command_output" checkout
git -C "$cloned" config user.email tests@example.test
git -C "$cloned" config user.name 'Second Device User'
printf 'Committed from another device\n' >"$cloned/from-device-two.txt"
git -C "$cloned" add from-device-two.txt
commit "$cloned" 'Second device change'
capture second sync --dry-run --verbose
succeeded
contains "$command_output" refs/heads/main
contains "$command_output" 'differing tips'
contains "$command_output" preflight
same_before_upload=$(git --git-dir "$test_root/repos/hosted.git" rev-parse refs/heads/main)
[ "$same_before_upload" = "$initial" ] || fail 'sync preview uploaded a changed branch'
capture second sync
succeeded
same_ref "$cloned" refs/heads/main
capture client sync --dry-run --verbose
[ "$command_status" -eq 3 ] || fail "outdated checkout must report rejected upload: $command_output"
contains "$command_output" refs/heads/main
contains "$command_output" preflight
capture second publish --dry-run "$cloned"
succeeded
contains "$command_output" hosted
capture second clone hosted "$duplicate"
succeeded
contains "$command_output" "$duplicate"
contains "$command_output" 'set-canonical'
git -C "$duplicate" branch secondary-only
capture second sync
succeeded
absent_ref refs/heads/secondary-only
capture second checkout list
succeeded
contains "$command_output" "$cloned"

# All methods of changing the main checkout must change the same selection.
# A configured discovery root then finds both copies but must obey that choice.
printf 'scan_root=%s\n' "$test_root/clones" >>"$second_config"
capture second register --repo "$duplicate" --replace-checkout
succeeded
git -C "$cloned" branch previous-main-only
capture second sync --dry-run --verbose
succeeded
contains "$command_output" "$duplicate"
contains "$command_output" refs/heads/secondary-only
lacks "$command_output" refs/heads/previous-main-only
capture second sync --scan --dry-run --verbose
succeeded
contains "$command_output" "$duplicate"
contains "$command_output" refs/heads/secondary-only
lacks "$command_output" refs/heads/previous-main-only
capture second sync --scan
succeeded
same_ref "$duplicate" refs/heads/secondary-only
absent_ref refs/heads/previous-main-only
capture second checkout list
succeeded
contains "$command_output" "$duplicate"
capture second status
attention
contains "$command_output" "$duplicate"

# The explicit selection command and both transfer replacement flags use that
# same inventory too. A replacement preview must not change the selection.
capture second checkout set-canonical hosted "$cloned"
succeeded
capture second sync --scan --dry-run --verbose
succeeded
contains "$command_output" "$cloned"
contains "$command_output" refs/heads/previous-main-only
capture second publish --yes --replace-checkout "$duplicate"
succeeded
capture second sync --repo "$cloned" --replace-checkout --dry-run --verbose
succeeded
capture second sync --dry-run --verbose
succeeded
contains "$command_output" "$duplicate"
lacks "$command_output" refs/heads/previous-main-only
capture second sync --repo "$cloned" --replace-checkout
succeeded
same_ref "$cloned" refs/heads/previous-main-only
capture second sync --scan --dry-run --verbose
succeeded
contains "$command_output" "$cloned"
capture second register --repo "$duplicate" --replace-checkout
succeeded

# Missing removable storage remains visible in all managed inventory views.
mv "$duplicate" "$test_root/offline-copy"
capture second status
[ "$command_status" -eq 3 ] || fail "missing managed path needs attention: $command_output"
contains "$command_output" hosted
contains "$command_output" "$duplicate"
capture second checkout list
attention
contains "$command_output" hosted
contains "$command_output" "$duplicate"
capture second sync --dry-run
[ "$command_status" -eq 3 ] || fail "missing managed path sync needs attention: $command_output"
contains "$command_output" "$duplicate"

# Legacy basename-only reports require explicit discovery; two equally valid
# matches never select a folder silently. A migration preview stays read-only.
mkdir "$test_root/legacy-config" "$test_root/legacy-folders" \
  "$test_root/legacy-folders/a" "$test_root/legacy-folders/b"
legacy_config="$test_root/legacy-config/client.ini"
write_config "$legacy_config" device-legacy 'Legacy Device'
legacy() { CKGIT_TEST_CLIENT_ID=device-legacy "$CKGIT" "$@" --config "$legacy_config"; }
for parent in a b; do
  checkout="$test_root/legacy-folders/$parent/legacy-copy"
  git clone -q --no-hardlinks --origin ckgit "$test_root/repos/hosted.git" "$checkout"
  git -C "$checkout" remote set-url ckgit ckgit@fixture:hosted.git
done
SSH_ORIGINAL_COMMAND="ckgit-rpc 1 register hosted $(encode_checkout_path legacy-copy)" "$CK_GIT_SHELL" \
  --client-id device-legacy --repo-root "$test_root/repos" \
  --control-socket "$test_root/control.sock" --state-root "$test_root/state" >"$test_root/legacy-register.out"
capture legacy status
[ "$command_status" -eq 3 ] || fail "legacy path needs migration advice: $command_output"
contains "$command_output" hosted
contains "$command_output" 'checkout migrate'
printf 'scan_root=%s\n' "$test_root/legacy-folders" >>"$legacy_config"
capture legacy checkout migrate
[ "$command_status" -eq 3 ] || fail "ambiguous migration must need attention: $command_output"
contains "$command_output" ambiguous
contains "$command_output" "$test_root/legacy-folders/a/legacy-copy"
contains "$command_output" "$test_root/legacy-folders/b/legacy-copy"
contains "$command_output" 'checkout set-canonical'
[ ! -e "$test_root/legacy-config/canonical.ini" ] || fail 'ambiguous migration chose a checkout'
rm -rf "$test_root/legacy-folders/b/legacy-copy"
capture legacy checkout migrate --dry-run
succeeded
contains "$command_output" 'Would manage hosted'
contains "$command_output" "$test_root/legacy-folders/a/legacy-copy"
[ ! -e "$test_root/legacy-config/canonical.ini" ] || fail 'migration preview changed management'
capture legacy checkout migrate
succeeded
contains "$command_output" 'Managed hosted'
capture legacy sync --dry-run
succeeded
contains "$command_output" "$test_root/legacy-folders/a/legacy-copy"

# End-to-end browser requests cover selected identity, recovery, documentation
# reading, and the semantic hooks used by the responsive layout.
base_url="http://127.0.0.1:$port"
request() {
  http_status=$(curl --path-as-is --max-time 5 --silent --show-error \
    -D "$test_root/headers" -o "$test_root/body" -w '%{http_code}' "$base_url$1")
  body=$(cat "$test_root/body")
}
page() {
  request "$1"
  [ "$http_status" -eq 200 ] || fail "$1 returned HTTP $http_status: $body"
  lacks "$body" '<script>'
}
attempt=0
while :; do
  page /project/hosted
  case "$body" in *'release/shared'*) break ;; esac
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || fail 'published refs did not appear in the index'
  sleep .1
done
for identity in heads/feature/shared tags/release/shared; do
  case "$identity" in heads/*) label='Branch feature/shared' ;; tags/*) label='Tag release/shared' ;; esac
  page "/project/hosted/tree/$identity:docs"
  contains "$body" "$label"
  contains "$body" "/project/hosted/blob/$identity:docs/guide.md"
  contains "$body" "/project/hosted/commits/$identity"
  contains "$body" "/project/hosted/calendar/$identity"
  contains "$body" "/project/hosted/overview/$identity"
  contains "$body" 'Jump to file'
  contains "$body" 'class="tree-switch"'
  contains "$body" 'class="tree-toggle"'
  contains "$body" '<details'
  page "/project/hosted/blob/$identity:docs/guide.md"
  contains "$body" "$label"
  contains "$body" '<strong>rendered guide</strong>'
  contains "$body" 'id="install-now"'
  contains "$body" "/project/hosted/source/$identity:docs/guide.md"
  contains "$body" "/project/hosted/blob/$identity:README.md#welcome"
  contains "$body" "/project/hosted/raw/$initial:docs/guide.md"
  contains "$body" '&lt;script&gt;never_run_this()'
  page "/project/hosted/source/$identity:docs/guide.md"
  contains "$body" "$label"
  contains "$body" 'id="L1"'
  contains "$body" "/project/hosted/blob/$identity:docs/guide.md"
  contains "$body" 'Wrap lines'
  page "/project/hosted/commits/$identity"
  contains "$body" "$label"
  contains "$body" 'class="commit-table"'
  contains "$body" 'class="commit-subject"'
  contains "$body" 'class="commit-id"'
  contains "$body" "/project/hosted/tree/$identity:"
  page "/project/hosted/calendar/$identity/2024/02"
  contains "$body" "$label"
  contains "$body" "/project/hosted/day/$identity/2024-02-29"
  contains "$body" '/project/hosted/calendar/heads/feature/shared/2024/02'
  contains "$body" '/project/hosted/calendar/tags/release/shared/2024/02'
  page "/project/hosted/day/$identity/2024-02-29"
  contains "$body" "$label"
  contains "$body" "/project/hosted/calendar/$identity/2024/02"
  contains "$body" "/project/hosted/tree/$initial:"
  page "/project/hosted/overview/$identity"
  contains "$body" "$label"
  contains "$body" "/project/hosted/tree/$identity:"
done

# A commit ID shared by several refs must stay an explicitly immutable snapshot.
page "/project/hosted/tree/$initial:docs"
contains "$body" "Commit $(printf '%.8s' "$initial")"
contains "$body" "/project/hosted/commits/$initial"
page "/project/hosted/commit/$initial"
contains "$body" "Commit $(printf '%.8s' "$initial")"
contains "$body" "/project/hosted/tree/$initial:"
contains "$body" "/project/hosted/calendar/$initial"

page '/project/hosted/blob/heads/feature/shared:README.md'
contains "$body" 'id="welcome"'
contains "$body" '/project/hosted/blob/heads/feature/shared:docs/guide.md#install-now'
lacks "$body" 'This maintenance comment must remain invisible'
request '/project/hosted/blob/heads/feature/shared:docs'
[ "$http_status" -eq 302 ] || fail 'extensionless directory link did not redirect to its tree'
contains "$(cat "$test_root/headers")" 'Location: /project/hosted/tree/heads/feature/shared:docs'
page '/project/hosted/tree/heads/feature/shared:docs'
contains "$body" 'Documentation'
page '/project/hosted/blob/heads/feature/shared:plain.txt'
contains "$body" 'id="L2"'
contains "$body" 'Wrap lines'

# Missing content still offers another ref and the correct parent/root folders.
request '/project/hosted/blob/tags/release/shared:docs/missing.md'
[ "$http_status" -eq 404 ] || fail 'missing file must retain its correct HTTP status'
contains "$body" 'Tag release/shared'
contains "$body" '/project/hosted/tree/tags/release/shared:docs'
contains "$body" '/project/hosted/tree/tags/release/shared:'
contains "$body" '<details'
contains "$body" '/project/hosted'
for route in '/project/empty' '/project/empty/tree/heads/main:' \
    '/project/empty/commits/heads/main' '/project/empty/calendar/heads/main' \
    '/project/empty/graph/heads/main'; do
  page "$route"
  contains "$body" 'No commits published yet'
  contains "$body" ckgit
  lacks "$body" 'href="/project/empty/tree/'
  lacks "$body" 'href="/project/empty/commits/'
done

echo 'UX journeys integration test passed'
