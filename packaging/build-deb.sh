#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Builds two Debian packages from a finished build directory:
#   ck-git-hosting_VERSION_ARCH.deb  server daemon, dispatcher, admin tool, hook,
#                                    systemd unit, sshd drop-in, server.ini
#   ckgit_VERSION_ARCH.deb           the client
# The server tree is produced by packaging/install.sh in staging mode so a
# package and a script installation lay out identical files.  Account
# creation, ownership, systemd, and sshd handling live in the maintainer
# scripts.  --stage-only prepares both package roots without dpkg-deb, which
# lets the layout be tested on a workstation.

set -eu

usage() {
  cat <<'USAGE'
Usage: build-deb.sh --build-dir DIR --output DIR [--version V] [--arch ARCH] [--stage-only]

  --build-dir DIR  build directory containing bin/ and hooks/ (make all)
  --output DIR     where the .deb files (or staged roots) are written
  --version V      package version; defaults to the VERSION file plus a
                   +YYYYMMDD.HHMM.<commit> build stamp so every rebuild upgrades
  --arch ARCH      Debian architecture; defaults to dpkg --print-architecture
  --stage-only     write DIR/ck-git-hosting and DIR/ckgit package roots only
Environment: CKGIT_MAINTAINER overrides the Maintainer field.
USAGE
}

build_dir=''
output=''
version=''
arch=''
stage_only=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) [ $# -ge 2 ] || { usage >&2; exit 2; }; build_dir=$2; shift 2 ;;
    --output) [ $# -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
    --version) [ $# -ge 2 ] || { usage >&2; exit 2; }; version=$2; shift 2 ;;
    --arch) [ $# -ge 2 ] || { usage >&2; exit 2; }; arch=$2; shift 2 ;;
    --stage-only) stage_only=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

fail() {
  printf 'build-deb.sh: %s\n' "$1" >&2
  exit 1
}

[ -n "$build_dir" ] && [ -n "$output" ] || { usage >&2; exit 2; }
case "$output" in
  /*) ;;
  *) fail "--output must be an absolute path" ;;
esac
script_dir=$(cd "$(dirname "$0")" && pwd)
source_root=$(cd "$script_dir/.." && pwd)
if [ -z "$version" ]; then
  [ -f "$source_root/VERSION" ] || fail "missing VERSION file"
  version=$(tr -d '[:space:]' <"$source_root/VERSION")
  # A development build must sort above the plain release version and above
  # any earlier development build, otherwise dpkg treats a rebuilt package
  # with unchanged sources as already installed.  Releases pass --version.
  stamp=$(date -u +%Y%m%d.%H%M)
  commit=$(git -C "$source_root" rev-parse --short HEAD 2>/dev/null || echo nogit)
  version="$version+$stamp.$commit"
fi
case "$version" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) fail "version must look like MAJOR.MINOR.PATCH: $version" ;;
esac
case "$version" in
  *[!A-Za-z0-9.+~]*) fail "version may contain only letters, digits, '.', '+', and '~': $version" ;;
esac
if [ -z "$arch" ]; then
  command -v dpkg >/dev/null 2>&1 || fail "--arch is required when dpkg is unavailable"
  arch=$(dpkg --print-architecture)
fi
if [ "$stage_only" -eq 0 ]; then
  command -v dpkg-deb >/dev/null 2>&1 || fail "dpkg-deb is required unless --stage-only is given"
fi
maintainer=${CKGIT_MAINTAINER:-'C. Klukas <christian.klukas@gmail.com>'}

mkdir -p "$output"
server_root="$output/ck-git-hosting"
client_root="$output/ckgit"
rm -rf "$server_root" "$client_root"

write_copyright() {  # write_copyright PACKAGE
  install -d -m 0755 "$1/usr/share/doc/$2"
  cat >"$1/usr/share/doc/$2/copyright" <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ck-git-hosting

Files: *
Copyright: 2026 C. Klukas
License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
COPYRIGHT
  chmod 0644 "$1/usr/share/doc/$2/copyright"
}

# ---- server package -------------------------------------------------------
sh "$script_dir/install.sh" --build-dir "$build_dir" --staging "$server_root" --yes >/dev/null
# dpkg owns these differently from a script installation:
#  - authorized_keys is created by postinst, never shipped, so purge and
#    upgrade cannot touch device keys by accident;
#  - the units belong in the packaged systemd directory, not /etc.
rm -f "$server_root/etc/ck-git-hosting/authorized_keys"
install -d -m 0755 "$server_root/usr/lib/systemd/system"
mv "$server_root/etc/systemd/system/ck-git-hosting.service" "$server_root/usr/lib/systemd/system/ck-git-hosting.service"
mv "$server_root/etc/systemd/system/ck-ci-runner.service" "$server_root/usr/lib/systemd/system/ck-ci-runner.service"
mv "$server_root/etc/systemd/system/ck-pages.service" "$server_root/usr/lib/systemd/system/ck-pages.service"
rmdir "$server_root/etc/systemd/system" "$server_root/etc/systemd"
write_copyright "$server_root" ck-git-hosting
install -d -m 0755 "$server_root/DEBIAN"
cat >"$server_root/DEBIAN/control" <<CONTROL
Package: ck-git-hosting
Version: $version
Section: vcs
Priority: optional
Architecture: $arch
Maintainer: $maintainer
Depends: git, openssh-server, systemd
Recommends: ckgit
Description: private LAN Git control plane over OpenSSH
 ck-git-hosting keeps Git transport in Git and OpenSSH and adds a small,
 dependency-free control plane: a hardened daemon with a same-user control
 socket and loopback dashboard, a restricted SSH dispatcher used as a forced
 command for each device key, a compiled shared receive hook, and an
 administrative tool for project creation and device pairing.  An opt-in,
 sandboxed CI runner executes each project's .ckgit/ci.yml workflow on push
 and shows the results read-only in the dashboard.
CONTROL
printf '%s\n' /etc/ck-git-hosting/server.ini /etc/ssh/sshd_config.d/ck-git-hosting.conf >"$server_root/DEBIAN/conffiles"
cat >"$server_root/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
case "$1" in
  configure)
    if ! getent passwd ckgit >/dev/null 2>&1; then
      useradd --system --user-group --home-dir /var/lib/ck-git-hosting --no-create-home \
        --shell /bin/sh --comment 'ck-git-hosting service' ckgit
    fi
    chown ckgit:ckgit /srv/ck-git-hosting/repos /var/lib/ck-git-hosting /var/lib/ck-git-hosting/state /var/lib/ck-git-hosting/ci-build /var/lib/ck-git-hosting/pages
    chmod 0750 /srv/ck-git-hosting/repos /var/lib/ck-git-hosting
    chmod 0700 /var/lib/ck-git-hosting/state /var/lib/ck-git-hosting/ci-build /var/lib/ck-git-hosting/pages
    chown root:ckgit /etc/ck-git-hosting/server.ini
    chmod 0640 /etc/ck-git-hosting/server.ini
    if [ ! -e /etc/ck-git-hosting/authorized_keys ]; then
      : >/etc/ck-git-hosting/authorized_keys
      chown root:root /etc/ck-git-hosting/authorized_keys
      chmod 0644 /etc/ck-git-hosting/authorized_keys
    fi
    if [ -d /run/systemd/system ]; then
      systemctl daemon-reload || true
      for unit in ck-git-hosting.service ck-ci-runner.service; do
        systemctl enable "$unit" >/dev/null 2>&1 || true
        if systemctl is-active --quiet "$unit"; then
          systemctl restart "$unit" || true
        else
          systemctl start "$unit" || true
        fi
      done
    fi
    if command -v sshd >/dev/null 2>&1 && [ -e /etc/ssh/sshd_config.d/ck-git-hosting.conf ]; then
      if sshd -t; then
        if [ -d /run/systemd/system ]; then
          systemctl reload ssh.service >/dev/null 2>&1 || systemctl reload sshd.service >/dev/null 2>&1 || true
        fi
      else
        mv /etc/ssh/sshd_config.d/ck-git-hosting.conf /etc/ssh/sshd_config.d/ck-git-hosting.conf.disabled
        echo "ck-git-hosting: sshd rejected the drop-in; it was renamed to .disabled and sshd was not reloaded" >&2
      fi
    fi
    echo "ck-git-hosting: pair a device with: ckgit-admin authorized-key --client-id ID --public-key KEY.pub >> /etc/ck-git-hosting/authorized_keys"
    ;;
esac
exit 0
POSTINST
cat >"$server_root/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
set -e
case "$1" in
  remove|deconfigure)
    if [ -d /run/systemd/system ]; then
      systemctl disable --now ck-pages.service >/dev/null 2>&1 || true
      systemctl disable --now ck-ci-runner.service >/dev/null 2>&1 || true
      systemctl disable --now ck-git-hosting.service >/dev/null 2>&1 || true
    fi
    ;;
esac
exit 0
PRERM
cat >"$server_root/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
case "$1" in
  remove)
    if [ -d /run/systemd/system ]; then
      systemctl daemon-reload || true
    fi
    ;;
  purge)
    rm -rf /etc/ck-git-hosting /var/lib/ck-git-hosting
    rm -f /etc/ssh/sshd_config.d/ck-git-hosting.conf.disabled
    if [ -d /run/systemd/system ]; then
      systemctl daemon-reload || true
      if command -v sshd >/dev/null 2>&1 && sshd -t 2>/dev/null; then
        systemctl reload ssh.service >/dev/null 2>&1 || systemctl reload sshd.service >/dev/null 2>&1 || true
      fi
    fi
    echo "ck-git-hosting: bare repositories in /srv/ck-git-hosting/repos and the ckgit account were kept; remove them manually if wanted" >&2
    ;;
esac
exit 0
POSTRM
chmod 0755 "$server_root/DEBIAN/postinst" "$server_root/DEBIAN/prerm" "$server_root/DEBIAN/postrm"
chmod 0644 "$server_root/DEBIAN/control" "$server_root/DEBIAN/conffiles"

# ---- client package -------------------------------------------------------
install -d -m 0755 "$client_root/usr/bin"
install -m 0755 "$build_dir/bin/ckgit" "$client_root/usr/bin/ckgit"
write_copyright "$client_root" ckgit
install -d -m 0755 "$client_root/DEBIAN"
cat >"$client_root/DEBIAN/control" <<CONTROL
Package: ckgit
Version: $version
Section: vcs
Priority: optional
Architecture: $arch
Maintainer: $maintainer
Depends: git, openssh-client
Description: client for a private ck-git-hosting Git server
 ckgit discovers local Git working trees without modifying them, compares
 refs against the paired server, publishes and clones projects, registers
 checkouts, and performs a safe non-force sync of committed branches and
 tags over the restricted SSH command.
CONTROL
chmod 0644 "$client_root/DEBIAN/control"

if [ "$stage_only" -eq 1 ]; then
  printf 'Staged package roots: %s %s\n' "$server_root" "$client_root"
  exit 0
fi

write_md5sums() {
  (cd "$1" && find . -type f ! -path './DEBIAN/*' | sed 's|^\./||' | LC_ALL=C sort | xargs md5sum >DEBIAN/md5sums)
  chmod 0644 "$1/DEBIAN/md5sums"
}
write_md5sums "$server_root"
write_md5sums "$client_root"
dpkg-deb --root-owner-group --build "$server_root" "$output/ck-git-hosting_${version}_${arch}.deb" >/dev/null
dpkg-deb --root-owner-group --build "$client_root" "$output/ckgit_${version}_${arch}.deb" >/dev/null
rm -rf "$server_root" "$client_root"
printf 'Built %s and %s\n' "$output/ck-git-hosting_${version}_${arch}.deb" "$output/ckgit_${version}_${arch}.deb"
