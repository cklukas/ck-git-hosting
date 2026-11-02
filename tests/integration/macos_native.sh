#!/bin/sh
# Root-only end-to-end installation and qualification in an isolated slot.
# No broker is started and no hosted job is claimed. Existing slots are untouched.
set -eu
[ "$(uname -s)" = Darwin ] || { echo 'macOS required' >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo 'Run with sudo: temporary account and launchd qualification require root.' >&2; exit 1; }
[ "$#" = 3 ] || { echo 'Usage: macos_native.sh ABS_BUILD_BIN BROKER_USER NEW_TASK_DIRECTORY' >&2; exit 1; }
bin=$1
broker=$2
task=$3
case "$task" in /Volumes/S8T/tmp/*) volume=/Volumes/S8T;; /Volumes/PRO-BLADE/tmp/*) volume=/Volumes/PRO-BLADE;; *) echo 'Select an approved scratch volume' >&2; exit 1;; esac
[ "$(stat -f %d "$volume")" != "$(stat -f %d /Volumes)" ] || { echo 'Scratch volume is not mounted' >&2; exit 1; }
[ ! -e "$task" ] || { echo 'Choose a new task directory' >&2; exit 1; }
mkdir -m 755 "$task"
mkdir -m 700 "$task/tmp"
export TMPDIR="$task/tmp"
broker_uid=$(id -u "$broker")
broker_gid=$(id -g "$broker")
[ "$broker_uid" != 0 ] || { echo 'Broker must be unprivileged' >&2; exit 1; }
name="native-test-$$"
account="_ckgit_$name"
state="/Library/Application Support/ckgit-native/$name"
label="org.ckgit.native.$name"
helper_dir="/Library/PrivilegedHelperTools/$label"
service="/Library/LaunchDaemons/$label.plist"
broker_service="/Library/LaunchDaemons/org.ckgit.runner.$name.plist"
# A failed setup can have encountered an existing slot with this PID-derived
# name. Never reconcile or delete it unless this setup completed successfully.
setup_complete=0
cleanup() {
  result=$?
  trap - EXIT HUP INT TERM
  if [ "$setup_complete" = 1 ] && [ -f "$state/config" ]; then
    /bin/launchctl bootout "system/$label" >/dev/null 2>&1 || true
    # No cleanup of accounts or workspace until the UID is proven empty.
    if ! "$bin/ckgit-macos-native" reconcile "$state"; then
      echo "Recovery failed; isolated slot preserved for inspection: $state" >&2
      exit 1
    fi
    /usr/bin/dscl . -delete "/Users/$account"
    /usr/bin/dscl . -delete "/Groups/$account"
    rm -f "$service" "$broker_service" "/var/run/ckgit-native-$name.sock" "/var/run/ckgit-native-$name.sock.broker-lock"
    rm -rf "$state" "$helper_dir" "$task/jobs" "$task/jobs.ckgit-owner" "$task/ckgit-service-$name"
  elif [ -e "$state" ]; then
    echo "Existing or partial setup preserved for inspection: $state" >&2
    exit 1
  fi
  exit "$result"
}
trap cleanup EXIT HUP INT TERM
/usr/bin/ssh-keygen -q -t ed25519 -N '' -f "$task/key"
printf '127.0.0.1 %s\n' "$(cat "$task/key.pub")" > "$task/known_hosts"
chown "$broker_uid:$broker_gid" "$task/key" "$task/key.pub" "$task/known_hosts"
cat > "$task/runner.ini" <<CONFIG
schema_version=1
server=ckgit@127.0.0.1
name=$name
ci_build_root=$task/jobs
identity=$task/key
known_hosts=$task/known_hosts
ci_allow_network=false
CONFIG
"$bin/ckgit-admin" runner macos-native setup --config "$task/runner.ini" --broker-user "$broker" --work-root "$task/jobs" --dry-run
"$bin/ckgit-admin" runner macos-native setup --config "$task/runner.ini" --broker-user "$broker" --work-root "$task/jobs"
setup_complete=1
[ -f "$state/qualified" ]
# Neither setup nor qualification may start the SSH broker.
if /bin/launchctl print "system/org.ckgit.runner.$name" >/dev/null 2>&1; then
  echo 'Setup unexpectedly started the broker' >&2
  exit 1
fi
/usr/bin/plutil -lint "$service" "$broker_service"
echo 'PASS admin setup, identity separation, process cleanup, sandbox restrictions and restart recovery; no broker started.'
