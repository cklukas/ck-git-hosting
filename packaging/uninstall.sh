#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Removes the ck-git-hosting service, binaries, hook, and sshd drop-in.  Git
# repositories, configuration, metadata state, and the service account are
# preserved unless their explicit removal flag is given.

set -eu

usage() {
  cat <<'USAGE'
Usage: uninstall.sh [--staging DIR] [--dry-run] [--yes]
                    [--remove-state] [--remove-repositories] [--remove-account]

  --staging DIR          operate beneath DIR only; skips systemd, sshd, and accounts
  --dry-run              print the planned changes and exit
  --yes                  apply without an interactive confirmation
  --remove-state         also delete /etc/ck-git-hosting and /var/lib/ck-git-hosting
  --remove-repositories  also delete /srv/ck-git-hosting (every bare repository)
  --remove-account       also delete the ckgit system account
USAGE
}

staging=''
dry_run=0
assume_yes=0
remove_state=0
remove_repositories=0
remove_account=0

while [ $# -gt 0 ]; do
  case "$1" in
    --staging) [ $# -ge 2 ] || { usage >&2; exit 2; }; staging=$2; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --yes) assume_yes=1; shift ;;
    --remove-state) remove_state=1; shift ;;
    --remove-repositories) remove_repositories=1; shift ;;
    --remove-account) remove_account=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

fail() {
  printf 'uninstall.sh: %s\n' "$1" >&2
  exit 1
}

if [ -n "$staging" ]; then
  case "$staging" in
    /*) ;;
    *) fail "--staging must be an absolute path" ;;
  esac
else
  [ "$(id -u)" -eq 0 ] || fail "a real removal must run as root; use --staging DIR to test"
fi

bin_dir="$staging/usr/bin"
hook_parent="$staging/usr/lib/ck-git-hosting"
etc_dir="$staging/etc/ck-git-hosting"
srv_dir="$staging/srv/ck-git-hosting"
lib_dir="$staging/var/lib/ck-git-hosting"
unit_path="$staging/etc/systemd/system/ck-git-hosting.service"
sshd_dropin="$staging/etc/ssh/sshd_config.d/ck-git-hosting.conf"
mode=plan

act() {
  description=$1
  shift
  if [ "$mode" = plan ]; then
    printf '  + %s\n' "$description"
  else
    printf '  * %s\n' "$description"
    "$@"
  fi
}

remove_path() {
  rm -rf "$1"
}

reload_sshd() {
  if sshd -t; then
    systemctl reload ssh.service
  else
    fail "sshd -t failed after removing the drop-in; review /etc/ssh before reloading"
  fi
}

steps() {
  had_unit=0
  [ -e "$unit_path" ] && had_unit=1
  if [ -z "$staging" ] && [ "$had_unit" -eq 1 ]; then
    act 'systemctl disable --now ck-git-hosting.service' systemctl disable --now ck-git-hosting.service
  fi
  [ "$had_unit" -eq 1 ] && act "remove $unit_path" remove_path "$unit_path"
  if [ -z "$staging" ] && [ "$had_unit" -eq 1 ]; then
    act 'systemctl daemon-reload' systemctl daemon-reload
  fi
  for binary in ck-git-hostingd ck-git-shell ckgit-admin; do
    [ -e "$bin_dir/$binary" ] && act "remove $bin_dir/$binary" remove_path "$bin_dir/$binary"
  done
  [ -e "$hook_parent" ] && act "remove $hook_parent" remove_path "$hook_parent"
  if [ -e "$sshd_dropin" ]; then
    act "remove $sshd_dropin" remove_path "$sshd_dropin"
    [ -z "$staging" ] && act 'validate sshd configuration and reload ssh.service' reload_sshd
  fi
  if [ "$remove_state" -eq 1 ]; then
    [ -e "$etc_dir" ] && act "remove $etc_dir (configuration and authorized keys)" remove_path "$etc_dir"
    [ -e "$lib_dir" ] && act "remove $lib_dir (checkout metadata and event log)" remove_path "$lib_dir"
  else
    printf '  = keep %s and %s\n' "$etc_dir" "$lib_dir"
  fi
  if [ "$remove_repositories" -eq 1 ]; then
    [ -e "$srv_dir" ] && act "remove $srv_dir (ALL bare repositories)" remove_path "$srv_dir"
  else
    printf '  = keep %s\n' "$srv_dir"
  fi
  if [ -z "$staging" ]; then
    if [ "$remove_account" -eq 1 ] && getent passwd ckgit >/dev/null 2>&1; then
      act 'delete system account ckgit' userdel ckgit
    else
      printf '  = keep account ckgit\n'
    fi
  fi
  return 0
}

printf 'ck-git-hosting removal plan'
[ -n "$staging" ] && printf ' (staging beneath %s)' "$staging"
printf ':\n'
mode=plan
steps
if [ "$dry_run" -eq 1 ]; then
  printf 'Dry run: nothing was changed.\n'
  exit 0
fi
if [ "$assume_yes" -ne 1 ]; then
  if [ -t 0 ]; then
    printf 'Apply these changes? [y/N] '
    read -r answer
    case "$answer" in
      y|Y|yes|YES) ;;
      *) printf 'Aborted: nothing was changed.\n'; exit 1 ;;
    esac
  else
    fail "no terminal for confirmation; pass --yes to apply or --dry-run to review"
  fi
fi
printf 'Applying:\n'
mode=apply
steps
printf 'Removal complete.\n'
