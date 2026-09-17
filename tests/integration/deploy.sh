#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Exercises ck-git-hosting-deploy's read-only paths against a fabricated
# release store: selection, checksum verification, --if-new, and the
# CI-busy/--wait guard -- all via --dry-run, which needs no root, no dpkg,
# and no systemctl, so this runs on a workstation exactly like every other
# integration test. The install/health/rollback path needs a real Debian
# host with systemd and is exercised by hand (see docs/operations/04-ci-cd.md
# and the WP5 work package notes for the Lima VM verification performed
# before this shipped).

set -eu

test_root_parent=${CKGIT_TEST_ROOT:-${TMPDIR:-/tmp}}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *) echo "TMPDIR must be beneath $test_root_parent" >&2; exit 1 ;;
esac
test_root=$(mktemp -d "$test_root_parent/ckdeploy.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
fail() { echo "$1" >&2; exit 1; }

packaging=$(cd "$(dirname "$0")/../../packaging" && pwd)
deploy="$packaging/ck-git-hosting-deploy"

# A portable (macOS and Linux) "did anything under here change" snapshot:
# every file's path and byte count. Avoids stat(1), whose format flags
# differ between BSD and GNU.
snapshot() {
  find "$1" -type f | LC_ALL=C sort | while IFS= read -r entry; do
    printf '%s %s\n' "$(wc -c <"$entry" | tr -d ' ')" "$entry"
  done
}

state="$test_root/state"
mkdir -p "$state/releases/demo/v1"
release_dir="$state/releases/demo/v1"

# A fake release: a tarball with one dummy .deb inside, plus a sidecar
# recording the tarball's own real sha256 -- the command re-derives and
# compares this itself, never trusting a value it did not just compute. Named
# for the real architecture when dpkg is present (a real target always has
# it), so the deploy command's own arch-specific match finds it; "all" is a
# harmless placeholder on a workstation with no dpkg, where the deploy
# command itself falls back to matching any architecture.
if command -v dpkg >/dev/null 2>&1; then
  fixture_arch=$(dpkg --print-architecture)
else
  fixture_arch=all
fi
deb_name="ck-git-hosting_1_${fixture_arch}.deb"
fixture_src="$test_root/fixture-src"
mkdir -p "$fixture_src"
printf 'not a real package\n' >"$fixture_src/$deb_name"
( cd "$fixture_src" && tar -cf "$release_dir/packages.tar" "$deb_name" )
cp "$release_dir/packages.tar" "$test_root/packages.tar.good"  # for the tamper/restore step below
sha256=$(sha256sum "$release_dir/packages.tar" 2>/dev/null | cut -d' ' -f1)
[ -n "$sha256" ] || sha256=$(shasum -a 256 "$release_dir/packages.tar" | cut -d' ' -f1)
bytes=$(wc -c <"$release_dir/packages.tar" | tr -d ' ')
cat >"$release_dir/packages.ini" <<INI
schema_version=1
name=packages
bytes=$bytes
sha256=$sha256
created_epoch=2000000000
expires_epoch=0
note_hex=
INI
cat >"$release_dir/release.ini" <<INI
schema_version=1
tag=v1
commit=0000000000000000000000000000000000000001
created_epoch=2000000000
notes_hex=
INI

config="$test_root/server.ini"
printf 'schema_version=1\nstate_root=%s\n' "$state" >"$config"
deploy_dir="$test_root/deploy"

# A plain dry run needs no root, selects the newest (only) release, verifies
# its checksum, prints a plan naming the tag and the packaged .deb, and
# leaves the fixture and --deploy-dir untouched.
before=$(snapshot "$state")
out=$("$deploy" --dry-run --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir")
after=$(snapshot "$state")
[ "$before" = "$after" ] || fail "a dry run modified files under state_root"
[ ! -e "$deploy_dir" ] || fail "a dry run created --deploy-dir"
printf '%s\n' "$out" | grep -q 'v1' || fail "the plan does not name the selected tag"
printf '%s\n' "$out" | grep -q "$deb_name" || fail "the plan does not name the packaged .deb"

# A tampered asset is refused, naming the mismatch, before anything would be
# installed; restoring the original bytes lets the remaining checks proceed.
printf 'x' >>"$release_dir/packages.tar"
if "$deploy" --dry-run --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir" \
    >"$test_root/tamper.out" 2>&1; then
  fail "a tampered asset was accepted"
fi
grep -qi 'sha256 mismatch' "$test_root/tamper.out" || fail "the refusal does not name a sha256 mismatch"
cp "$test_root/packages.tar.good" "$release_dir/packages.tar"

# An unknown asset name is refused with a clear reason, not a crash.
if "$deploy" --dry-run --config "$config" --project demo --asset nonexistent --deploy-dir "$deploy_dir" \
    >"$test_root/missing-asset.out" 2>&1; then
  fail "a nonexistent asset name was accepted"
fi
grep -qi 'nonexistent.ini' "$test_root/missing-asset.out" || fail "the refusal does not name the missing sidecar"

# --if-new: once the tag is recorded as deployed, a later run exits 0
# immediately, saying so, without re-verifying anything.
mkdir -p "$deploy_dir"
printf 'v1\n' >"$deploy_dir/deployed-tag"
out=$("$deploy" --dry-run --if-new --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir")
printf '%s\n' "$out" | grep -qi 'already deployed' || fail "--if-new did not report the release as already deployed"

# An in-progress CI build is refused immediately, explaining why...
mkdir -p "$state/ci/working"
: >"$state/ci/working/x.ini"
if "$deploy" --dry-run --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir" \
    >"$test_root/busy.out" 2>&1; then
  fail "a busy runner was not refused"
fi
grep -qi 'CI build is running' "$test_root/busy.out" || fail "the refusal does not explain why"

# ...but --wait polls until it clears, then proceeds.
( sleep 0.5; rm -f "$state/ci/working/x.ini" ) &
waiter=$!
"$deploy" --dry-run --wait 3 --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir" \
  >/dev/null
wait "$waiter"

# Without --dry-run, a non-root caller is refused before touching anything.
if [ "$(id -u)" -ne 0 ]; then
  if "$deploy" --config "$config" --project demo --asset packages --deploy-dir "$deploy_dir" \
      >"$test_root/noroot.out" 2>&1; then
    fail "a non-root, non-dry-run invocation was accepted"
  fi
  grep -qi 'root' "$test_root/noroot.out" || fail "the refusal does not mention root"
fi

# --help works offline, without --config or root.
"$deploy" --help >/dev/null

echo "deploy command integration OK"
