#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Installs the ck-git-hosting server as a hardened systemd service on a Debian
# or similar Linux host.  Every change is listed before anything is applied;
# --dry-run stops after the list.  --staging DIR lays out the identical tree
# beneath DIR without touching accounts, systemd, or sshd, which is how the
# installer is exercised by the test suite on a workstation.

set -eu

usage() {
  cat <<'USAGE'
Usage: install.sh --build-dir DIR [--staging DIR] [--dry-run] [--yes]
                  [--http-port PORT] [--no-service] [--no-sshd]

  --build-dir DIR   build directory containing bin/ and hooks/ (make all)
  --staging DIR     install beneath DIR only; skips account, systemd, and sshd
  --dry-run         print the planned changes and exit
  --yes             apply without an interactive confirmation
  --http-port PORT  enable the loopback dashboard in a newly written server.ini
  --no-service      install the unit file but do not enable or start it
  --no-sshd         do not write the sshd drop-in for the ckgit account
USAGE
}

build_dir=''
staging=''
dry_run=0
assume_yes=0
http_port=''
configure_service=1
configure_sshd=1

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) [ $# -ge 2 ] || { usage >&2; exit 2; }; build_dir=$2; shift 2 ;;
    --staging) [ $# -ge 2 ] || { usage >&2; exit 2; }; staging=$2; shift 2 ;;
    --http-port) [ $# -ge 2 ] || { usage >&2; exit 2; }; http_port=$2; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --yes) assume_yes=1; shift ;;
    --no-service) configure_service=0; shift ;;
    --no-sshd) configure_sshd=0; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

fail() {
  printf 'install.sh: %s\n' "$1" >&2
  exit 1
}

[ -n "$build_dir" ] || { usage >&2; exit 2; }
for required in bin/ck-git-hostingd bin/ck-git-shell bin/ckgit-admin bin/ck-ci-runnerd bin/ck-pagesd hooks/post-receive; do
  [ -f "$build_dir/$required" ] || fail "missing build product: $build_dir/$required (run make all first)"
done
if [ -n "$http_port" ]; then
  case "$http_port" in
    ''|*[!0-9]*) fail "--http-port must be numeric" ;;
  esac
  [ "$http_port" -le 65535 ] || fail "--http-port must be 0 to 65535"
fi
if [ -n "$staging" ]; then
  case "$staging" in
    /*) ;;
    *) fail "--staging must be an absolute path" ;;
  esac
else
  [ "$(id -u)" -eq 0 ] || fail "a real installation must run as root; use --staging DIR to test"
  [ "$(uname -s)" = Linux ] || fail "the packaged service targets Linux with systemd"
fi

# The installed layout.  packaging/uninstall.sh and include/ckgit/install_layout.hpp
# describe the same paths.
bin_dir="$staging/usr/bin"
hook_dir="$staging/usr/lib/ck-git-hosting/hooks"
etc_dir="$staging/etc/ck-git-hosting"
srv_dir="$staging/srv/ck-git-hosting"
repo_root="$srv_dir/repos"
lib_dir="$staging/var/lib/ck-git-hosting"
state_root="$lib_dir/state"
ci_build_root="$lib_dir/ci-build"
ci_cache_root="$lib_dir/ci-cache"
pages_root="$lib_dir/pages"
unit_dir="$staging/etc/systemd/system"
unit_path="$unit_dir/ck-git-hosting.service"
sshd_dir="$staging/etc/ssh/sshd_config.d"
sshd_dropin="$sshd_dir/ck-git-hosting.conf"
server_ini="$etc_dir/server.ini"
authorized_keys="$etc_dir/authorized_keys"
script_dir=$(cd "$(dirname "$0")" && pwd)
unit_source="$script_dir/systemd/ck-git-hosting.service"
[ -f "$unit_source" ] || fail "missing unit template: $unit_source"
runner_unit_path="$unit_dir/ck-ci-runner.service"
runner_unit_source="$script_dir/systemd/ck-ci-runner.service"
[ -f "$runner_unit_source" ] || fail "missing unit template: $runner_unit_source"
pages_unit_path="$unit_dir/ck-pages.service"
pages_unit_source="$script_dir/systemd/ck-pages.service"
[ -f "$pages_unit_source" ] || fail "missing unit template: $pages_unit_source"
deploy_source="$script_dir/ck-git-hosting-deploy"
[ -f "$deploy_source" ] || fail "missing script: $deploy_source"

as_root=0
[ -z "$staging" ] && [ "$(id -u)" -eq 0 ] && as_root=1
mode=plan

# act DESCRIPTION COMMAND...: list in plan mode, list and execute in apply mode.
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

install_dir() {  # install_dir MODE OWNER GROUP PATH
  if [ "$as_root" -eq 1 ]; then
    install -d -m "$1" -o "$2" -g "$3" "$4"
  else
    install -d -m "$1" "$4"
  fi
}

install_file() {  # install_file MODE OWNER GROUP SOURCE DESTINATION
  if [ "$as_root" -eq 1 ]; then
    install -m "$1" -o "$2" -g "$3" "$4" "$5"
  else
    install -m "$1" "$4" "$5"
  fi
}

write_server_ini() {
  {
    printf '%s\n' \
      '# ck-git-hosting server configuration, schema 1.' \
      '# The daemon reads this file when it starts; restart the service after a change.' \
      'schema_version=1' \
      'repo_root=/srv/ck-git-hosting/repos' \
      'control_socket=/run/ck-git-hosting/control.sock' \
      'state_root=/var/lib/ck-git-hosting/state' \
      'hook_directory=/usr/lib/ck-git-hosting/hooks' \
      'ci_build_root=/var/lib/ck-git-hosting/ci-build' \
      'ci_cache_root=/var/lib/ck-git-hosting/ci-cache' \
      'pages_root=/var/lib/ck-git-hosting/pages'
    if [ -n "$http_port" ]; then
      printf 'http_port=%s\n' "$http_port"
    else
      printf '%s\n' '# Uncomment to serve the read-only dashboard on 127.0.0.1 only:' '#http_port=8420'
    fi
    printf '%s\n' '# Advertise the SSH destination used by visitors, not the loopback dashboard address:' \
      '#ssh_clone_target=ckgit@git-server'
    printf '%s\n' '# Uncomment and restart ck-pages.service to serve project sites on the LAN' \
      '# (the service is already enabled; it stays inactive until this is set):' \
      '#pages_http_port=8421'
  } >"$server_ini.new"
  mv "$server_ini.new" "$server_ini"
  chmod 0640 "$server_ini"
  [ "$as_root" -eq 1 ] && chown root:ckgit "$server_ini"
  return 0
}

write_authorized_keys() {
  : >"$authorized_keys.new"
  mv "$authorized_keys.new" "$authorized_keys"
  chmod 0644 "$authorized_keys"
  [ "$as_root" -eq 1 ] && chown root:root "$authorized_keys"
  return 0
}

write_sshd_dropin() {
  printf '%s\n' \
    '# Installed by ck-git-hosting.  Only the ckgit service account is affected.' \
    '# Device keys live in the root-owned /etc/ck-git-hosting/authorized_keys and' \
    '# each entry carries a restricted forced command; see docs/operations.' \
    'Match User ckgit' \
    '    AuthorizedKeysFile /etc/ck-git-hosting/authorized_keys' \
    '    PubkeyAuthentication yes' \
    '    PasswordAuthentication no' \
    '    KbdInteractiveAuthentication no' \
    '    AllowTcpForwarding no' \
    '    AllowAgentForwarding no' \
    '    AllowStreamLocalForwarding no' \
    '    X11Forwarding no' \
    '    PermitTTY no' \
    '    PermitTunnel no' \
    '    GatewayPorts no' \
    '    PermitUserRC no' >"$sshd_dropin.new"
  mv "$sshd_dropin.new" "$sshd_dropin"
  chmod 0644 "$sshd_dropin"
  [ "$as_root" -eq 1 ] && chown root:root "$sshd_dropin"
  return 0
}

validate_sshd() {
  if ! sshd -t; then
    rm -f "$sshd_dropin"
    fail "sshd rejected the drop-in; it was removed again and sshd was not reloaded"
  fi
}

create_account() {
  useradd --system --user-group --home-dir /var/lib/ck-git-hosting --no-create-home \
    --shell /bin/sh --comment 'ck-git-hosting service' ckgit
}

steps() {
  if [ -z "$staging" ]; then
    if getent passwd ckgit >/dev/null 2>&1; then
      printf '  = keep existing account ckgit\n'
    else
      act 'create locked system account ckgit (home /var/lib/ck-git-hosting, shell /bin/sh, no password)' create_account
    fi
  fi
  act "create $etc_dir (root:root 0755)" install_dir 0755 root root "$etc_dir"
  act "create $srv_dir (root:root 0755)" install_dir 0755 root root "$srv_dir"
  act "create $repo_root (ckgit:ckgit 0750)" install_dir 0750 ckgit ckgit "$repo_root"
  act "create $lib_dir (ckgit:ckgit 0750)" install_dir 0750 ckgit ckgit "$lib_dir"
  act "create $state_root (ckgit:ckgit 0700)" install_dir 0700 ckgit ckgit "$state_root"
  act "create $state_root/runtime (ckgit:ckgit 0755)" install_dir 0755 ckgit ckgit "$state_root/runtime"
  act "create $ci_build_root (ckgit:ckgit 0700)" install_dir 0700 ckgit ckgit "$ci_build_root"
  act "create $ci_cache_root (ckgit:ckgit 0700)" install_dir 0700 ckgit ckgit "$ci_cache_root"
  act "create $pages_root (ckgit:ckgit 0700)" install_dir 0700 ckgit ckgit "$pages_root"
  act "create $hook_dir (root:root 0755)" install_dir 0755 root root "$hook_dir"
  [ -n "$staging" ] && act "create $bin_dir" install_dir 0755 root root "$bin_dir"
  for binary in ck-git-hostingd ck-git-shell ckgit-admin ck-ci-runnerd ck-pagesd; do
    act "install $bin_dir/$binary (root:root 0755)" install_file 0755 root root "$build_dir/bin/$binary" "$bin_dir/$binary"
  done
  act "install $bin_dir/ck-git-hosting-deploy (root:root 0755)" install_file 0755 root root "$deploy_source" "$bin_dir/ck-git-hosting-deploy"
  act "install $hook_dir/post-receive (root:root 0755)" install_file 0755 root root "$build_dir/hooks/post-receive" "$hook_dir/post-receive"
  if [ -e "$server_ini" ]; then
    printf '  = keep existing %s\n' "$server_ini"
  else
    act "write $server_ini (root:ckgit 0640)" write_server_ini
  fi
  if [ -e "$authorized_keys" ]; then
    printf '  = keep existing %s\n' "$authorized_keys"
  else
    act "create empty $authorized_keys (root:root 0644)" write_authorized_keys
  fi
  [ -n "$staging" ] && act "create $unit_dir" install_dir 0755 root root "$unit_dir"
  act "install $unit_path (root:root 0644)" install_file 0644 root root "$unit_source" "$unit_path"
  act "install $runner_unit_path (root:root 0644)" install_file 0644 root root "$runner_unit_source" "$runner_unit_path"
  act "install $pages_unit_path (root:root 0644)" install_file 0644 root root "$pages_unit_source" "$pages_unit_path"
  if [ -z "$staging" ] && [ "$configure_service" -eq 1 ]; then
    act 'systemctl daemon-reload' systemctl daemon-reload
    act 'systemctl enable --now ck-git-hosting.service' systemctl enable --now ck-git-hosting.service
    act 'systemctl enable --now ck-ci-runner.service' systemctl enable --now ck-ci-runner.service
    # ck-pages.service's own ExecCondition (ck-pagesd check) keeps it cleanly
    # inactive, not restart-looping, until pages_http_port is also set; enable
    # it unconditionally, the same as the other two, so it starts on its own
    # the moment that key is uncommented and the service is restarted.
    act 'systemctl enable --now ck-pages.service' systemctl enable --now ck-pages.service
  fi
  if [ "$configure_sshd" -eq 1 ]; then
    if [ -n "$staging" ]; then
      act "create $sshd_dir" install_dir 0755 root root "$sshd_dir"
      act "write $sshd_dropin (root:root 0644)" write_sshd_dropin
    elif [ -d /etc/ssh/sshd_config.d ] && grep -q 'sshd_config\.d' /etc/ssh/sshd_config 2>/dev/null; then
      act "write $sshd_dropin (Match User ckgit: root-owned AuthorizedKeysFile, no forwarding, no PTY)" write_sshd_dropin
      act 'validate the complete sshd configuration with sshd -t' validate_sshd
      act 'systemctl reload ssh.service' systemctl reload ssh.service
    else
      printf '  ! /etc/ssh/sshd_config does not include sshd_config.d; add the Match User ckgit block manually (see docs/operations)\n'
    fi
  fi
}

printf 'ck-git-hosting installation plan'
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
printf 'Installation complete.\n'
if [ -z "$staging" ]; then
  printf 'Pair a device: ckgit-admin authorized-key --client-id ID --public-key KEY.pub >> %s\n' "$authorized_keys"
fi
