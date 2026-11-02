#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Run inside the prepared macOS guest, as root, with the guest binary supplied
# from the host's explicitly read-only setup share.
set -eu
LC_ALL=C
export LC_ALL
if [ "$#" -lt 3 ]; then
  printf '%s\n' 'usage: sudo install-vm-guest.sh GUEST-BINARY GUEST-USER EMPTY-WORK-ROOT [NAME=VALUE ...]' >&2
  exit 2
fi
guest_binary=$1
guest_account=$2
guest_work=$3
shift 3
guest_install='/Library/Application Support/ck-git-hosting-vm'
guest_plist='/Library/LaunchDaemons/org.ckgit.vm-guest.plist'
if [ "$(id -u)" -ne 0 ]; then printf '%s\n' 'run as root inside the guest' >&2; exit 1; fi
case "$guest_binary" in /*) ;; *) printf '%s\n' 'guest binary path must be absolute' >&2; exit 1;; esac
case "$guest_work" in /*) ;; *) printf '%s\n' 'work root must be absolute' >&2; exit 1;; esac
case "$guest_account" in ''|*[!a-zA-Z0-9_.-]*) printf '%s\n' 'invalid guest user' >&2; exit 1;; esac
guest_uid=$(id -u "$guest_account")
[ "$guest_uid" -ne 0 ] || { printf '%s\n' 'guest user must be unprivileged' >&2; exit 1; }
[ -f "$guest_binary" ] && [ ! -L "$guest_binary" ] || { printf '%s\n' 'guest binary must be a regular file' >&2; exit 1; }
[ -d "$guest_work" ] && [ ! -L "$guest_work" ] || { printf '%s\n' 'work root must be a real directory' >&2; exit 1; }
[ "$(cd "$guest_work" && pwd -P)" = "$guest_work" ] || { printf '%s\n' 'work root may not contain symlinks or relative components' >&2; exit 1; }
[ "$(stat -f '%u %Lp' "$guest_work")" = "$guest_uid 700" ] || { printf '%s\n' 'work root must be owned by the guest user with mode 0700' >&2; exit 1; }
[ -z "$(ls -A "$guest_work")" ] || { printf '%s\n' 'work root must be empty' >&2; exit 1; }
TMPDIR=$guest_work
export TMPDIR
[ ! -e "$guest_install" ] && [ ! -L "$guest_install" ] && [ ! -e "$guest_plist" ] && [ ! -L "$guest_plist" ] || {
  printf '%s\n' 'guest service already exists; refusing to overwrite it' >&2; exit 1;
}
umask 077
mkdir -m 755 "$guest_install"
cleanup() {
  if [ -e "$guest_plist" ] || [ -L "$guest_plist" ]; then /bin/rm "$guest_plist"; fi
  if [ -e "$guest_install/ckgit-macos-vm-guest" ] || [ -L "$guest_install/ckgit-macos-vm-guest" ]; then /bin/rm "$guest_install/ckgit-macos-vm-guest"; fi
  rmdir "$guest_install" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
/usr/bin/install -o root -g wheel -m 755 "$guest_binary" "$guest_install/ckgit-macos-vm-guest"
/usr/bin/plutil -create xml1 "$guest_plist"
/usr/bin/plutil -insert Label -string org.ckgit.vm-guest "$guest_plist"
/usr/bin/plutil -insert UserName -string "$guest_account" "$guest_plist"
/usr/bin/plutil -insert RunAtLoad -bool YES "$guest_plist"
/usr/bin/plutil -insert KeepAlive -dictionary "$guest_plist"
/usr/bin/plutil -insert KeepAlive.SuccessfulExit -bool NO "$guest_plist"
/usr/bin/plutil -insert ThrottleInterval -integer 10 "$guest_plist"
/usr/bin/plutil -insert EnvironmentVariables -dictionary "$guest_plist"
/usr/bin/plutil -insert EnvironmentVariables.TMPDIR -string "$guest_work" "$guest_plist"
guest_env_count=0
for guest_env in "$@"; do
  guest_env_name=${guest_env%%=*}
  guest_env_value=${guest_env#*=}
  [ "$guest_env_name" != "$guest_env" ] || { printf '%s\n' 'tool environment needs NAME=VALUE' >&2; exit 1; }
  case "$guest_env_name" in [A-Z]*) ;; *) printf '%s\n' 'invalid tool environment name' >&2; exit 1;; esac
  case "$guest_env_name" in *[!A-Z0-9_]*|PATH|HOME|TMPDIR|SHELL|CI|LANG|ENV|BASH_ENV|CDPATH|IFS|CKGIT_*|DYLD_*|LD_*|GIT_*|SSH_*|LC_*)
    printf '%s\n' 'invalid or reserved tool environment name' >&2; exit 1;;
  esac
  guest_cr=$(printf '\r')
  case "$guest_env_value" in ''|*"$guest_cr"*|*'
'*) printf '%s\n' 'invalid tool environment value' >&2; exit 1;; esac
  guest_env_count=$((guest_env_count + 1))
  [ "$guest_env_count" -le 32 ] && [ "${#guest_env_name}" -le 128 ] && [ "${#guest_env_value}" -le 4096 ] || {
    printf '%s\n' 'tool environment exceeds limits' >&2; exit 1;
  }
  /usr/bin/plutil -insert "EnvironmentVariables.CKGIT_VM_ENV_$guest_env_name" -string "$guest_env_value" "$guest_plist"
done
/usr/bin/plutil -insert ProgramArguments -array "$guest_plist"
/usr/bin/plutil -insert ProgramArguments.0 -string "$guest_install/ckgit-macos-vm-guest" "$guest_plist"
/usr/bin/plutil -insert ProgramArguments.1 -string --exchange "$guest_plist"
/usr/bin/plutil -insert ProgramArguments.2 -string '/Volumes/My Shared Files' "$guest_plist"
/usr/bin/plutil -insert ProgramArguments.3 -string --work-root "$guest_plist"
/usr/bin/plutil -insert ProgramArguments.4 -string "$guest_work" "$guest_plist"
/usr/bin/plutil -lint "$guest_plist" >/dev/null
chown root:wheel "$guest_plist"
chmod 644 "$guest_plist"
[ -z "$(ls -A "$guest_work")" ] || { printf '%s\n' 'installation left the guest work root nonempty' >&2; exit 1; }
trap - EXIT HUP INT TERM
printf '%s\n' 'Guest service installed. Shut down this setup VM before starting CI.'
