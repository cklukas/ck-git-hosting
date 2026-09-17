#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Exercises the packaging scripts in staging mode, the configuration-driven
# daemon start, and the authorized-key generator without touching the host.

set -eu

# The Makefile exports the approved build root; on the development Mac that is
# /Volumes/PRO-BLADE/tmp, in CI it is the runner's temporary directory.
test_root_parent=${CKGIT_TEST_ROOT:-/Volumes/PRO-BLADE/tmp}
case "${TMPDIR:-}" in
  "$test_root_parent"/*) ;;
  *)
    echo "TMPDIR must be beneath $test_root_parent" >&2
    exit 1
    ;;
esac

# Unix-domain sockets have a short path limit; keep the fixture shallow.
test_root=$(mktemp -d "$test_root_parent/ckp.XXXXXX")
server_pid=''

cleanup() {
  if [ -n "$server_pid" ]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

fail() {
  echo "$1" >&2
  exit 1
}

has_mode() {  # has_mode PATH OCTAL
  find "$1" -maxdepth 0 -perm "$2" | grep -q .
}

packaging=$(cd "$(dirname "$0")/../../packaging" && pwd)
staging="$test_root/root"

# A dry run lists every change and creates nothing.
sh "$packaging/install.sh" --build-dir "$CKGIT_BUILD_DIR" --staging "$staging" --dry-run >"$test_root/plan.out"
[ ! -e "$staging" ] || fail "dry run created files"
grep -q 'Dry run: nothing was changed' "$test_root/plan.out"
grep -q '+ install .*/usr/bin/ck-git-hostingd (root:root 0755)' "$test_root/plan.out"
grep -q '+ write .*/etc/ssh/sshd_config.d/ck-git-hosting.conf' "$test_root/plan.out"

# Without a terminal and without --yes the installer refuses to apply.
if sh "$packaging/install.sh" --build-dir "$CKGIT_BUILD_DIR" --staging "$staging" </dev/null >/dev/null 2>&1; then
  fail "installer applied changes without confirmation"
fi
[ ! -e "$staging" ] || fail "refused installation still created files"

sh "$packaging/install.sh" --build-dir "$CKGIT_BUILD_DIR" --staging "$staging" --yes --http-port 8420 \
  >"$test_root/apply.out"
grep -q 'Installation complete' "$test_root/apply.out"
for executable in usr/bin/ck-git-hostingd usr/bin/ck-git-shell usr/bin/ckgit-admin \
  usr/bin/ck-ci-runnerd usr/bin/ck-pagesd usr/lib/ck-git-hosting/hooks/post-receive; do
  [ -x "$staging/$executable" ] || fail "missing executable $executable"
done
server_ini="$staging/etc/ck-git-hosting/server.ini"
authorized_keys="$staging/etc/ck-git-hosting/authorized_keys"
unit="$staging/etc/systemd/system/ck-git-hosting.service"
runner_unit="$staging/etc/systemd/system/ck-ci-runner.service"
pages_unit="$staging/etc/systemd/system/ck-pages.service"
dropin="$staging/etc/ssh/sshd_config.d/ck-git-hosting.conf"
[ -f "$server_ini" ] || fail "server.ini was not written"
[ -f "$authorized_keys" ] || fail "authorized_keys was not created"
[ ! -s "$authorized_keys" ] || fail "authorized_keys must start empty"
grep -q '^http_port=8420$' "$server_ini"
grep -q '^hook_directory=/usr/lib/ck-git-hosting/hooks$' "$server_ini"
grep -q '^ci_build_root=/var/lib/ck-git-hosting/ci-build$' "$server_ini"
grep -q '^ci_cache_root=/var/lib/ck-git-hosting/ci-cache$' "$server_ini"
grep -q '^pages_root=/var/lib/ck-git-hosting/pages$' "$server_ini"
grep -q '^ExecStart=/usr/bin/ck-git-hostingd --config /etc/ck-git-hosting/server.ini$' "$unit"
grep -q '^User=ckgit$' "$unit"
grep -q '^ProtectSystem=strict$' "$unit"
grep -q '^ReadWritePaths=/var/lib/ck-git-hosting /srv/ck-git-hosting/repos$' "$unit"
grep -q '^ExecStart=/usr/bin/ck-ci-runnerd serve --config /etc/ck-git-hosting/server.ini$' "$runner_unit"
grep -q '^User=ckgit$' "$runner_unit"
grep -q '^RestrictNamespaces=user mnt net$' "$runner_unit"
grep -q '^ExecCondition=/usr/bin/ck-pagesd check --config /etc/ck-git-hosting/server.ini$' "$pages_unit"
grep -q '^ExecStart=/usr/bin/ck-pagesd serve --config /etc/ck-git-hosting/server.ini$' "$pages_unit"
grep -q '^User=ckgit$' "$pages_unit"
grep -q '^Match User ckgit$' "$dropin"
grep -q '^    AuthorizedKeysFile /etc/ck-git-hosting/authorized_keys$' "$dropin"
grep -q '^    PermitTTY no$' "$dropin"
has_mode "$staging/var/lib/ck-git-hosting/state" 0700 || fail "state root must be mode 0700"
has_mode "$staging/var/lib/ck-git-hosting/ci-build" 0700 || fail "CI build root must be mode 0700"
has_mode "$staging/var/lib/ck-git-hosting/ci-cache" 0700 || fail "CI cache root must be mode 0700"
has_mode "$staging/var/lib/ck-git-hosting/pages" 0700 || fail "pages root must be mode 0700"
has_mode "$staging/srv/ck-git-hosting/repos" 0750 || fail "repository root must be mode 0750"
has_mode "$server_ini" 0640 || fail "server.ini must be mode 0640"
has_mode "$authorized_keys" 0644 || fail "authorized_keys must be mode 0644"

# The daemon parses the staged configuration without opening any path.
check_output=$("$CKGIT_HOSTINGD" --config "$server_ini" --check)
[ "$check_output" = 'schema_version=1
repo_root=/srv/ck-git-hosting/repos
control_socket=/run/ck-git-hosting/control.sock
state_root=/var/lib/ck-git-hosting/state
hook_directory=/usr/lib/ck-git-hosting/hooks
http_port=8420' ] || fail "unexpected --check output: $check_output"

# Reinstalling preserves administrator-owned files.
printf '# customized by the administrator\n' >>"$server_ini"
printf 'ssh-ed25519 AAAAexample placeholder\n' >"$authorized_keys"
sh "$packaging/install.sh" --build-dir "$CKGIT_BUILD_DIR" --staging "$staging" --yes >"$test_root/reinstall.out"
grep -q '= keep existing .*server.ini' "$test_root/reinstall.out"
grep -q '# customized by the administrator' "$server_ini"
grep -q 'placeholder' "$authorized_keys"

# Configuration file and explicit path options are mutually exclusive.
set +e
"$CKGIT_HOSTINGD" --config "$server_ini" --repo-root "$test_root" --check >/dev/null 2>&1
mixed_status=$?
set -e
[ "$mixed_status" -eq 2 ] || fail "mixed configuration sources were not rejected as usage"
printf 'schema_version=1\nrepo_root=relative/path\ncontrol_socket=/tmp/x.sock\n' >"$test_root/bad.ini"
set +e
"$CKGIT_HOSTINGD" --config "$test_root/bad.ini" --check >/dev/null 2>&1
bad_status=$?
set -e
[ "$bad_status" -eq 1 ] || fail "relative repo_root was not rejected"
printf 'schema_version=1\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\nlisten=0.0.0.0\n' >"$test_root/unknown.ini"
if "$CKGIT_HOSTINGD" --config "$test_root/unknown.ini" --check >/dev/null 2>&1; then
  fail "unknown server configuration field was accepted"
fi

# A real daemon starts from a configuration file alone.
mkdir "$test_root/repos" "$test_root/state" "$test_root/hooks"
chmod 0700 "$test_root/state"
cp "$staging/usr/lib/ck-git-hosting/hooks/post-receive" "$test_root/hooks/post-receive"
chmod 0755 "$test_root/hooks/post-receive"
printf '%s\n' 'schema_version=1' "repo_root=$test_root/repos" "control_socket=$test_root/control.sock" \
  "state_root=$test_root/state" "hook_directory=$test_root/hooks" >"$test_root/server.ini"
"$CKGIT_HOSTINGD" --config "$test_root/server.ini" >"$test_root/server.log" 2>&1 &
server_pid=$!
attempt=0
while [ ! -S "$test_root/control.sock" ]; do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    sed -n '1,40p' "$test_root/server.log" >&2
    fail "daemon exited before its control socket was ready"
  fi
  attempt=$((attempt + 1))
  [ "$attempt" -lt 20 ] || fail "control socket did not become ready"
  sleep 1
done
created=$(SSH_ORIGINAL_COMMAND='ckgit-rpc 1 create delta main' "$CK_GIT_SHELL" \
  --client-id mac-studio --repo-root "$test_root/repos" --control-socket "$test_root/control.sock")
[ "$created" = 'ok created' ] || fail "configuration-driven daemon did not create a project"
[ "$(git --git-dir "$test_root/repos/delta.git" config --get core.hooksPath)" = "$test_root/hooks" ]
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=''

# Pairing prints one restricted authorized_keys line and rejects unsafe input.
key_data='AAAAC3NzaC1lZDI1NTE5AAAAIGb6Gz7pRt8bC1pT3VbwBgV6h4v2HkQx1y7z8Q0m8ZrA'
printf 'ssh-ed25519 %s user@example.test\n' "$key_data" >"$test_root/device.pub"
line=$("$CKGIT_ADMIN" authorized-key --client-id mac-studio --public-key "$test_root/device.pub")
expected="restrict,command=\"/usr/bin/ck-git-shell --client-id mac-studio --repo-root /srv/ck-git-hosting/repos --control-socket /run/ck-git-hosting/control.sock --state-root /var/lib/ck-git-hosting/state\" ssh-ed25519 $key_data mac-studio"
[ "$line" = "$expected" ] || fail "unexpected authorized_keys line: $line"
printf 'ssh-dss %s legacy\n' "$key_data" >"$test_root/legacy.pub"
if "$CKGIT_ADMIN" authorized-key --client-id mac-studio --public-key "$test_root/legacy.pub" >/dev/null 2>&1; then
  fail "unsupported key type was accepted"
fi
printf 'command="/bin/sh" ssh-ed25519 %s injected\n' "$key_data" >"$test_root/options.pub"
if "$CKGIT_ADMIN" authorized-key --client-id mac-studio --public-key "$test_root/options.pub" >/dev/null 2>&1; then
  fail "key line with options prefix was accepted"
fi
if "$CKGIT_ADMIN" authorized-key --client-id '-bad' --public-key "$test_root/device.pub" >/dev/null 2>&1; then
  fail "option-like client ID was accepted"
fi
if "$CKGIT_ADMIN" authorized-key --client-id mac-studio --public-key "$test_root/device.pub" \
  --repo-root '/srv/ck git' >/dev/null 2>&1; then
  fail "path with whitespace was embedded in a forced command"
fi

# Package staging mirrors the script installation with the two documented
# differences, and the release archive is a valid --build-dir.
sh "$packaging/build-deb.sh" --build-dir "$CKGIT_BUILD_DIR" --output "$test_root/deb" --arch arm64 --stage-only >/dev/null
[ -x "$test_root/deb/ck-git-hosting/usr/bin/ck-git-hostingd" ] || fail "staged server package lacks the daemon"
[ -x "$test_root/deb/ck-git-hosting/usr/bin/ck-ci-runnerd" ] || fail "staged server package lacks the CI runner"
[ -f "$test_root/deb/ck-git-hosting/usr/lib/systemd/system/ck-git-hosting.service" ] || fail "packaged unit is not under /usr/lib/systemd/system"
[ -f "$test_root/deb/ck-git-hosting/usr/lib/systemd/system/ck-ci-runner.service" ] || fail "packaged CI runner unit is not under /usr/lib/systemd/system"
[ -x "$test_root/deb/ck-git-hosting/usr/bin/ck-pagesd" ] || fail "staged server package lacks the pages server"
[ -f "$test_root/deb/ck-git-hosting/usr/lib/systemd/system/ck-pages.service" ] || fail "packaged pages unit is not under /usr/lib/systemd/system"
[ ! -e "$test_root/deb/ck-git-hosting/etc/systemd" ] || fail "package must not ship the unit under /etc"
[ ! -e "$test_root/deb/ck-git-hosting/etc/ck-git-hosting/authorized_keys" ] || fail "package must not ship authorized_keys"
grep -q '^Package: ck-git-hosting$' "$test_root/deb/ck-git-hosting/DEBIAN/control"
grep -q '^Version: ' "$test_root/deb/ck-git-hosting/DEBIAN/control"
# WP6/D7: a snapshot build's default version uses '~', which dpkg orders
# below the plain release of the same VERSION -- so a real release package
# always outranks any earlier development build, never the reverse.
staged_version=$(sed -n 's/^Version: //p' "$test_root/deb/ck-git-hosting/DEBIAN/control")
case "$staged_version" in
  *~*) ;;
  *) fail "a snapshot build's default version does not use '~': $staged_version" ;;
esac
if command -v dpkg >/dev/null 2>&1; then
  dpkg --compare-versions "$staged_version" lt "${staged_version%%~*}" ||
    fail "a snapshot version does not sort below the plain release: $staged_version"
fi
grep -q '^Architecture: arm64$' "$test_root/deb/ck-git-hosting/DEBIAN/control"
grep -q '^/etc/ck-git-hosting/server.ini$' "$test_root/deb/ck-git-hosting/DEBIAN/conffiles"
grep -q '^/etc/ssh/sshd_config.d/ck-git-hosting.conf$' "$test_root/deb/ck-git-hosting/DEBIAN/conffiles"
[ -x "$test_root/deb/ck-git-hosting/DEBIAN/postinst" ] || fail "postinst must be executable"
sh -n "$test_root/deb/ck-git-hosting/DEBIAN/postinst"
sh -n "$test_root/deb/ck-git-hosting/DEBIAN/prerm"
sh -n "$test_root/deb/ck-git-hosting/DEBIAN/postrm"
[ -x "$test_root/deb/ckgit/usr/bin/ckgit" ] || fail "staged client package lacks ckgit"
grep -q '^Package: ckgit$' "$test_root/deb/ckgit/DEBIAN/control"
[ -f "$test_root/deb/ckgit/usr/share/doc/ckgit/copyright" ] || fail "client package lacks a copyright file"
sh "$packaging/build-tarball.sh" --build-dir "$CKGIT_BUILD_DIR" --output "$test_root/tar" --platform test-server >/dev/null
sh "$packaging/build-tarball.sh" --build-dir "$CKGIT_BUILD_DIR" --output "$test_root/tar" --platform test-client --client-only >/dev/null
version=$(tr -d '[:space:]' <"$packaging/../VERSION")
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/packaging/install.sh$"
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/hooks/post-receive$"
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/bin/ck-ci-runnerd$"
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/packaging/systemd/ck-ci-runner.service$"
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/bin/ck-pagesd$"
tar -tzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" | grep -q "^ck-git-hosting-$version-test-server/packaging/systemd/ck-pages.service$"
tar -tzf "$test_root/tar/ckgit-$version-test-client.tar.gz" | grep -q "^ckgit-$version-test-client/bin/ckgit$"
if tar -tzf "$test_root/tar/ckgit-$version-test-client.tar.gz" | grep -q 'ck-git-hostingd'; then
  fail "client archive must not contain server binaries"
fi
if tar -tzf "$test_root/tar/ckgit-$version-test-client.tar.gz" | grep -q 'ck-ci-runnerd'; then
  fail "client archive must not contain the CI runner"
fi
if tar -tzf "$test_root/tar/ckgit-$version-test-client.tar.gz" | grep -q 'ck-pagesd'; then
  fail "client archive must not contain the pages server"
fi
mkdir "$test_root/unpacked"
tar -xzf "$test_root/tar/ck-git-hosting-$version-test-server.tar.gz" -C "$test_root/unpacked"
sh "$test_root/unpacked/ck-git-hosting-$version-test-server/packaging/install.sh" \
  --build-dir "$test_root/unpacked/ck-git-hosting-$version-test-server" --staging "$test_root/from-archive" --yes >/dev/null
[ -x "$test_root/from-archive/usr/bin/ck-git-shell" ] || fail "archive is not a usable --build-dir"

# Removal keeps repositories, configuration, and state unless asked otherwise.
sh "$packaging/uninstall.sh" --staging "$staging" --yes >"$test_root/uninstall.out"
grep -q 'Removal complete' "$test_root/uninstall.out"
[ ! -e "$staging/usr/bin/ck-git-hostingd" ] || fail "daemon binary was not removed"
[ ! -e "$staging/usr/bin/ck-ci-runnerd" ] || fail "CI runner binary was not removed"
[ ! -e "$staging/usr/bin/ck-pagesd" ] || fail "pages server binary was not removed"
[ ! -e "$staging/usr/lib/ck-git-hosting" ] || fail "hook directory was not removed"
[ ! -e "$unit" ] || fail "unit file was not removed"
[ ! -e "$runner_unit" ] || fail "CI runner unit was not removed"
[ ! -e "$pages_unit" ] || fail "pages unit was not removed"
[ ! -e "$dropin" ] || fail "sshd drop-in was not removed"
[ -f "$server_ini" ] || fail "server.ini was removed without --remove-state"
[ -f "$authorized_keys" ] || fail "authorized_keys was removed without --remove-state"
[ -d "$staging/srv/ck-git-hosting/repos" ] || fail "repositories were removed without --remove-repositories"
[ -d "$staging/var/lib/ck-git-hosting/state" ] || fail "state was removed without --remove-state"
sh "$packaging/uninstall.sh" --staging "$staging" --yes --remove-state --remove-repositories >/dev/null
[ ! -e "$staging/etc/ck-git-hosting" ] || fail "configuration survived --remove-state"
[ ! -e "$staging/var/lib/ck-git-hosting" ] || fail "state survived --remove-state"
[ ! -e "$staging/srv/ck-git-hosting" ] || fail "repositories survived --remove-repositories"

echo "install integration test passed"
