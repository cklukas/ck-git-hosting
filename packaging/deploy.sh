#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# On-demand server self-deploy for ck-git-hosting. The project's own CI builds
# and tests the suite on every push and, on a deploy-* tag, publishes the built
# Debian packages (and a source tarball) as a durable release. Run this as root
# on the server to install a release's server package and restart the services,
# with a health check and automatic rollback to the last healthy package:
#
#   sudo ck-git-hosting-deploy            # install the newest release
#   sudo ck-git-hosting-deploy --tag deploy-7
#
# The security boundary: the CI step that produces the package is unprivileged
# and sandboxed; only this root command installs it, only from a durable release
# (a deploy-* tag), and never automatically.
set -eu

STATE_ROOT=${CK_STATE_ROOT:-/var/lib/ck-git-hosting/state}
DEPLOY_DIR=${CK_DEPLOY_DIR:-/var/lib/ck-git-hosting/deploy}
DASHBOARD_URL=${CK_DASHBOARD_URL:-http://127.0.0.1:8420/}
PROJECT=ck-git-hosting
RELEASES="$STATE_ROOT/releases/$PROJECT"
want_tag=''

while [ $# -gt 0 ]; do
  case "$1" in
    --tag) [ $# -ge 2 ] || { echo "usage: ck-git-hosting-deploy [--tag TAG]" >&2; exit 2; }; want_tag=$2; shift 2 ;;
    -h|--help) echo "usage: ck-git-hosting-deploy [--tag TAG]"; exit 0 ;;
    *) echo "ck-git-hosting-deploy: unknown option $1" >&2; exit 2 ;;
  esac
done

log() { printf '%s ck-deploy: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
[ "$(id -u)" -eq 0 ] || { echo "ck-git-hosting-deploy: must run as root (use sudo)" >&2; exit 1; }
mkdir -p "$DEPLOY_DIR"

# Choose the release to deploy: an explicit --tag, else the newest one with a
# committed release record.
tag="$want_tag"
if [ -z "$tag" ]; then
  for d in $(ls -1dt "$RELEASES"/*/ 2>/dev/null); do
    [ -f "$d/release.ini" ] || continue
    tag=$(basename "$d")
    break
  done
fi
[ -n "$tag" ] || { log "no ck-git-hosting release found under $RELEASES"; exit 1; }
[ -f "$RELEASES/$tag/release.ini" ] || { log "release $tag has no committed record"; exit 1; }

if [ "$tag" = "$(cat "$DEPLOY_DIR/deployed-tag" 2>/dev/null || echo)" ]; then
  log "release $tag is already deployed; nothing to do"
  exit 0
fi

# Never restart the runner out from under a build in progress.
if [ -n "$(ls -A "$STATE_ROOT/ci/working" 2>/dev/null || true)" ]; then
  log "a CI build is running; re-run this once the runner is idle so it is not interrupted"
  exit 1
fi

log "deploying release $tag"
work=$(mktemp -d "${TMPDIR:-/tmp}/ckdeploy.XXXXXX")
trap 'rm -rf "$work"' EXIT

asset="$RELEASES/$tag/release.tar"
[ -f "$asset" ] || { log "release $tag has no release.tar asset"; exit 1; }
tar -xf "$asset" -C "$work"
deb=$(find "$work" -name 'ck-git-hosting_*.deb' | head -1)
[ -n "$deb" ] || { log "release $tag carries no ck-git-hosting_*.deb"; exit 1; }

prev="$DEPLOY_DIR/deployed.deb"                   # last package confirmed healthy (rollback target)

install_deb() {                                  # postinst restarts hosting+runner, not pages
  dpkg -i "$1" >/dev/null 2>&1 || return 1
  systemctl restart ck-pages.service 2>/dev/null || true
}
healthy() {
  systemctl is-active --quiet ck-git-hosting.service ck-ci-runner.service ck-pages.service || return 1
  [ "$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "$DASHBOARD_URL" 2>/dev/null || echo 000)" = 200 ]
}
settle() { i=0; while [ "$i" -lt 20 ] && ! healthy; do sleep 1; i=$((i + 1)); done; }

install_deb "$deb" || log "dpkg -i reported an error for $tag"
settle

if healthy; then
  cp "$deb" "$prev"
  printf '%s\n' "$tag" >"$DEPLOY_DIR/deployed-tag"
  log "deployed $tag OK (dashboard 200, all services active)"
  exit 0
fi

log "release $tag did not come up healthy; rolling back"
if [ -f "$prev" ]; then
  install_deb "$prev" || true
  settle
  if healthy; then log "rolled back to the previous package OK"; else log "ROLLBACK FAILED - manual intervention needed"; fi
else
  log "no previous package to roll back to - manual intervention needed"
fi
exit 1
