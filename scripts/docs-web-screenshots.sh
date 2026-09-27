#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Render documentation screenshots from an isolated, disposable installation.
# No configured server, checkout inventory, or existing hosted repository is read.

set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: sh scripts/docs-web-screenshots.sh OUTPUT_DIRECTORY" >&2
  exit 2
fi
output=$1
: "${CKGIT_ADMIN:?set CKGIT_ADMIN to the just-built ckgit-admin}"
: "${CKGIT_HOSTINGD:?set CKGIT_HOSTINGD to the just-built ck-git-hostingd}"
: "${CK_CI_RUNNER:?set CK_CI_RUNNER to the just-built ck-ci-runnerd}"
: "${CKGIT_POST_RECEIVE:?set CKGIT_POST_RECEIVE to the just-built post-receive hook}"
for binary in "$CKGIT_ADMIN" "$CKGIT_HOSTINGD" "$CK_CI_RUNNER" "$CKGIT_POST_RECEIVE"; do
  [ -x "$binary" ] || { echo "Missing screenshot fixture binary: $binary" >&2; exit 1; }
done

browser=${CKGIT_DOCS_BROWSER:-}
if [ -z "$browser" ]; then
  for candidate in chromium chromium-browser google-chrome google-chrome-stable; do
    if command -v "$candidate" >/dev/null 2>&1; then
      browser=$(command -v "$candidate")
      break
    fi
  done
fi
[ -n "$browser" ] && [ -x "$browser" ] || {
  echo "A headless Chromium/Chrome executable is required for documentation screenshots." >&2
  exit 1
}

case "$(uname -s)" in
  Darwin)
    # The development Mac's project scratch must stay on the mounted blade.
    [ -d /Volumes/PRO-BLADE/tmp ] && mount | grep -q '^/dev/.* on /Volumes/PRO-BLADE ' || {
      echo "The /Volumes/PRO-BLADE temporary volume is not mounted." >&2
      exit 1
    }
    temporary_parent=/Volumes/PRO-BLADE/tmp
    ;;
  *)
    temporary_parent=${TMPDIR:-/tmp}
    [ -d "$temporary_parent" ] || { echo "Temporary directory does not exist: $temporary_parent" >&2; exit 1; }
    ;;
esac

started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
started_epoch=$(date +%s)
fixture=$(mktemp -d "$temporary_parent/ckdocs-web.XXXXXX")
server_pid=''
cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$fixture"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$fixture/tmp" "$fixture/home" "$fixture/xdg-config" "$fixture/xdg-cache" \
  "$fixture/repos" "$fixture/state" "$fixture/ci-build"
chmod 0700 "$fixture/state"
TMPDIR="$fixture/tmp"
HOME="$fixture/home"
XDG_CONFIG_HOME="$fixture/xdg-config"
XDG_CACHE_HOME="$fixture/xdg-cache"
GIT_CONFIG_NOSYSTEM=1
GIT_CONFIG_GLOBAL=/dev/null
export TMPDIR HOME XDG_CONFIG_HOME XDG_CACHE_HOME GIT_CONFIG_NOSYSTEM GIT_CONFIG_GLOBAL

echo "docs screenshots: start=$started browser=$browser output=$output"
sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}
echo "docs screenshots: backend_sha256=$(sha256_file "$CKGIT_HOSTINGD")"
echo "docs screenshots: runner_sha256=$(sha256_file "$CK_CI_RUNNER")"
echo "docs screenshots: fixture_sha256=$(sha256_file "$0") browser_version=$("$browser" --version)"

cat >"$fixture/server.ini" <<EOF
schema_version=1
repo_root=$fixture/repos
control_socket=$fixture/control.sock
state_root=$fixture/state
ci_build_root=$fixture/ci-build
EOF

seed_project() {
  name=$1
  work="$fixture/$name"
  git init -q --initial-branch=main "$work"
  mkdir -p "$work/.ckgit" "$work/src" "$work/docs"
  if [ "$name" = garden-notes ]; then
    cat >"$work/README.md" <<'EOF'
# Garden Notes

A tiny example project for keeping notes about plants and the weather.

The dashboard shows its source, commit history, CI runs, and downloadable
artifacts without requiring a second web service.

## Quick start

Run `sh scripts/check.sh` to verify the sample data.

See the [field guide](docs/field-guide.md) for the first entry.
EOF
    cat >"$work/docs/field-guide.md" <<'EOF'
# Field guide

The first seedlings are ready. Record a note whenever the weather changes.
EOF
    cat >"$work/src/plants.txt" <<'EOF'
tomato: sunny window
basil: warm and watered
rosemary: dry soil
EOF
    mkdir -p "$work/scripts"
    cat >"$work/scripts/check.sh" <<'EOF'
#!/bin/sh
set -eu
grep -q 'basil:' src/plants.txt
echo '3 garden notes verified'
EOF
    cat >"$work/.ckgit/ci.yml" <<'EOF'
version: 1
jobs:
  - name: verify
    steps:
      - name: build
        script: |
          mkdir -p dist
          cp src/plants.txt dist/garden-notes.txt
          echo 'Garden notes bundle built'
      - name: test
        script: |
          sh scripts/check.sh
          test -s dist/garden-notes.txt
          echo 'Bundle and sample data passed'
    artifacts:
      name: garden-notes
      paths: [dist]
EOF
  else
    cat >"$work/README.md" <<'EOF'
# Weather Widget

A second example project used to demonstrate CI feedback. Its sample test
deliberately fails so the project list shows a build that needs attention.

No external API or credentials are used.
EOF
    cat >"$work/src/forecast.txt" <<'EOF'
Monday: sunny
Tuesday: cloudy
EOF
    cat >"$work/.ckgit/ci.yml" <<'EOF'
version: 1
jobs:
  - name: verify
    steps:
      - name: build
        script: |
          test -s src/forecast.txt
          echo 'Forecast data loaded'
      - name: test
        script: |
          echo 'Checking for the missing Wednesday sample...'
          grep -q '^Wednesday:' src/forecast.txt
EOF
  fi
  git -C "$work" add .
  git -C "$work" -c user.name='Documentation Demo' -c user.email='docs@example.invalid' \
    -c commit.gpgsign=false commit -q -m "Add $name example"
  commit=$(git -C "$work" rev-parse HEAD)
  git init -q --bare --initial-branch=main "$fixture/repos/$name.git"
  git -C "$work" push -q "$fixture/repos/$name.git" main
  "$CKGIT_ADMIN" ci enable "$name" --config "$fixture/server.ini" >/dev/null
  printf '%040d %s refs/heads/main\n' 0 "$commit" | \
    CKGIT_STATE_ROOT="$fixture/state" CKGIT_CLIENT_ID=docs-demo \
    CKGIT_PROJECT_NAME="$name" CKGIT_REPOSITORY_ROOT="$fixture/repos" \
    "$CKGIT_POST_RECEIVE"
  "$CK_CI_RUNNER" serve --config "$fixture/server.ini" --once >"$fixture/$name-runner.log" 2>&1 || {
    cat "$fixture/$name-runner.log" >&2
    echo "The demo CI runner failed for $name" >&2
    exit 1
  }
}

seed_project garden-notes
seed_project weather-widget

garden_run_ini=$(find "$fixture/state/ci/runs/garden-notes" -name run.ini -print -quit)
weather_run_ini=$(find "$fixture/state/ci/runs/weather-widget" -name run.ini -print -quit)
[ -n "$garden_run_ini" ] && [ -n "$weather_run_ini" ] || {
  echo "Demo CI did not record both runs." >&2
  exit 1
}
grep -q '^status=success$' "$garden_run_ini"
grep -q '^status=failure$' "$weather_run_ini"
garden_run=$(basename "$(dirname "$garden_run_ini")")

"$CKGIT_HOSTINGD" --repo-root "$fixture/repos" --state-root "$fixture/state" \
  --control-socket "$fixture/control.sock" --http-port 0 \
  --ssh-clone-target ckgit@demo.invalid >"$fixture/server.log" 2>&1 &
server_pid=$!
attempt=0
port=''
while [ -z "$port" ]; do
  kill -0 "$server_pid" 2>/dev/null || { cat "$fixture/server.log" >&2; exit 1; }
  port=$(sed -n 's/^ck-git-hostingd: loopback HTTP ready on \([0-9][0-9]*\)$/\1/p' "$fixture/server.log")
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { echo "Demo dashboard did not start." >&2; exit 1; }
  [ -n "$port" ] || sleep .1
done
base="http://127.0.0.1:$port"

attempt=0
while :; do
  curl --max-time 4 --silent --show-error --fail "$base/" >"$fixture/index.html"
  if grep -q 'class="project-name"' "$fixture/index.html" &&
     grep -q 'ci-success' "$fixture/index.html" &&
     grep -q 'ci-failure' "$fixture/index.html"; then
    break
  fi
  attempt=$((attempt + 1))
  [ "$attempt" -lt 100 ] || { echo "Demo projects and CI runs were not indexed." >&2; exit 1; }
  sleep .1
done
python3 - "$fixture/index.html" <<'PY'
import re
import sys

html = open(sys.argv[1], encoding="utf-8").read()
names = re.findall(r'<td class="project-name"><a href="/project/([^"]+)">', html)
if sorted(names) != ["garden-notes", "weather-widget"]:
    raise SystemExit(f"Unexpected projects in screenshot fixture: {names!r}")
PY
for url in "/project/garden-notes" "/project/garden-notes/tree/heads/main:" \
  "/project/garden-notes/ci/$garden_run"; do
  curl --max-time 4 --silent --show-error --fail "$base$url" >"$fixture/page.html"
  grep -q 'Garden Notes\|garden-notes' "$fixture/page.html"
done

mkdir -p "$output"
capture() {
  name=$1
  route=$2
  height=$3
  target="$output/$name.png"
  rm -f "$target"
  "$browser" --headless --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --disable-background-networking --disable-extensions --no-first-run \
    --hide-scrollbars --force-device-scale-factor=1 --window-size=1440,"$height" \
    --virtual-time-budget=1500 --user-data-dir="$fixture/chrome-$name" \
    --screenshot="$target" "$base$route" >"$fixture/chrome-$name.log" 2>&1 || {
      cat "$fixture/chrome-$name.log" >&2
      echo "Chromium failed to capture $name" >&2
      exit 1
    }
  python3 - "$target" "$height" <<'PY'
import os
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as image:
    header = image.read(24)
if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
    raise SystemExit(f"Not a PNG screenshot: {path}")
width, height = struct.unpack(">II", header[16:24])
if (width, height) != (1440, int(sys.argv[2])) or os.stat(path).st_size < 4096:
    raise SystemExit(f"Incomplete screenshot: {path} ({width}x{height})")
PY
  echo "docs screenshots: $target sha256=$(sha256_file "$target")"
}

capture web-projects / 500
capture web-project /project/garden-notes 1160
capture web-files /project/garden-notes/tree/heads/main: 760
capture web-ci /project/garden-notes/ci/$garden_run 640

ended=$(date -u +%Y-%m-%dT%H:%M:%SZ)
echo "docs screenshots: end=$ended elapsed_seconds=$(($(date +%s) - started_epoch))"
