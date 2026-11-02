#!/bin/sh
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
set -eu
[ "$#" -eq 2 ] && [ -n "${TMPDIR:-}" ] || exit 2
runner=$1
admin=$2
task=$(mktemp -d "${TMPDIR%/}/vm-startup.XXXXXX")
cleanup() {
  for file in "$task/config" "$task/low-config" "$task/log" "$task/original" "$task/fake-ssh" "$task/build/keep" "$task/work/.ckgit-runner.lock" "$task/work/unknown" \
              "$task/base/Disk.img" "$task/base/AuxiliaryStorage" "$task/base/MachineIdentifier" "$task/base/HardwareModel"; do
    if [ -e "$file" ] || [ -L "$file" ]; then /bin/rm "$file"; fi
  done
  rmdir "$task/build" "$task/work" "$task/base" "$task" 2>/dev/null || true
}
trap cleanup EXIT
umask 077
mkdir "$task/base" "$task/work" "$task/build"
for file in Disk.img AuxiliaryStorage MachineIdentifier HardwareModel; do printf fixture > "$task/base/$file"; done
printf preserve > "$task/build/keep"
write_config() {
  printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\nssh_program=/usr/bin/false\nci_allow_network=false\nci_macos_vm_base=%s\nci_macos_vm_work_root=%s\nci_macos_vm_min_free_gib=1\n' \
    "$1" "$task/base" "$task/work" > "$task/config"
}
write_config "$task/base"
if "$runner" serve --remote --once --config "$task/config" > "$task/log" 2>&1; then
  printf '%s\n' 'overlapping VM and build roots were accepted' >&2; exit 1
fi
case "$(cat "$task/log")" in *'must be separate'*) ;; *) cat "$task/log" >&2; exit 1;; esac
[ "$(cat "$task/base/Disk.img")" = fixture ] || { printf '%s\n' 'VM base was changed' >&2; exit 1; }
[ -z "$(ls -A "$task/work")" ] || { printf '%s\n' 'invalid overlap touched the VM work root' >&2; exit 1; }
write_config "$task/missing-build-root"
if "$runner" serve --remote --once --config "$task/config" > "$task/log" 2>&1; then
  printf '%s\n' 'missing VM build root was accepted' >&2; exit 1
fi
[ ! -e "$task/missing-build-root" ] || { printf '%s\n' 'missing VM build root was created' >&2; exit 1; }
[ -z "$(ls -A "$task/work")" ] || { printf '%s\n' 'missing relay root touched the VM work root' >&2; exit 1; }
write_config "$task/build"
python3 - "$runner" "$task/config" "$task/build" "$task/work" <<'PY'
import fcntl, os, subprocess, sys
runner, config, relay, work = sys.argv[1:]
stale = os.path.join(work, 'job-' + 'a' * 32)
os.mkdir(stale, 0o700)
marker = os.path.join(stale, '.ckgit-workspace')
with open(marker, 'w') as f:
    f.write('ckgit-macos-vm-v1\nboot=00000000-0000-4000-8000-000000000000\n')
os.chmod(marker, 0o600)
fd = os.open(relay, os.O_RDONLY)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    result = subprocess.run([runner, 'serve', '--remote', '--once', '--config', config],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode == 0 or 'already in use' not in result.stdout:
        raise SystemExit('held relay lock did not stop startup: ' + result.stdout)
    if not os.path.isfile(marker):
        raise SystemExit('workspace recovery ran while relay root was locked')
finally:
    os.close(fd)
    if os.path.exists(marker): os.unlink(marker)
    if os.path.isdir(stale): os.rmdir(stale)
PY
if "$runner" serve --remote --once --config "$task/config" > "$task/log" 2>&1; then
  printf '%s\n' 'nonempty VM build root was accepted' >&2; exit 1
fi
case "$(cat "$task/log")" in *'VM relay root contains unknown data'*) ;; *) cat "$task/log" >&2; exit 1;; esac
[ "$(cat "$task/build/keep")" = preserve ] || { printf '%s\n' 'existing build-root data was changed' >&2; exit 1; }
[ "$(ls -A "$task/work")" = '.ckgit-runner.lock' ] || { printf '%s\n' 'preflight left a VM workspace behind' >&2; exit 1; }
# Once VM preflight is safe, recover only a marked relay from a crashed sink.
/bin/rm "$task/build/keep"
relay="$task/build/00000000000000000001-abcdabcd.relay"
mkdir -m 700 "$relay" "$relay/private-test"
printf 'ckgit-vm-relay-v1\n' > "$relay/.ckgit-vm-relay"
chmod 600 "$relay/.ckgit-vm-relay"
printf 'old relay log\n' > "$relay/private-test/0.log"
cat > "$task/fake-ssh" <<'EOF'
#!/bin/sh
for last; do :; done
case "$last" in
  *' hello '*) printf 'ok\nprotocol=2\n' ;;
  *' claim '*) printf 'none\n' ;;
  *) exit 1 ;;
esac
EOF
chmod 700 "$task/fake-ssh"
printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\nssh_program=%s\nci_allow_network=false\nci_macos_vm_base=%s\nci_macos_vm_work_root=%s\nci_macos_vm_min_free_gib=1\n' \
  "$task/build" "$task/fake-ssh" "$task/base" "$task/work" > "$task/config"
"$runner" serve --remote --once --config "$task/config" > "$task/log" 2>&1 || { cat "$task/log" >&2; exit 1; }
[ ! -e "$relay" ] && [ -z "$(ls -A "$task/build")" ] || { printf '%s\n' 'marked relay was not recovered' >&2; exit 1; }
# Switching an existing native installation must never publish an invalid
# mixed-backend configuration. Then verify an ordinary config can be updated.
printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\nci_allow_network=false\nci_macos_native_socket=%s\n' \
  "$task/build" "$task/native.sock" > "$task/config"
cp "$task/config" "$task/original"
if "$runner" configure-macos-vm --config "$task/config" --base "$task/base" --work-root "$task/work" --min-free-gib 1 > "$task/log" 2>&1; then
  printf '%s\n' 'native and VM backend were mixed' >&2; exit 1
fi
cmp -s "$task/config" "$task/original" || { printf '%s\n' 'rejected configuration was modified' >&2; exit 1; }
# Configuration must reject overlapping or missing relay roots without
# publishing a file that the runner will reject later.
printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\n' "$task/base" > "$task/config"
cp "$task/config" "$task/original"
if "$runner" configure-macos-vm --config "$task/config" --base "$task/base" --work-root "$task/work" --min-free-gib 1 > "$task/log" 2>&1; then
  printf '%s\n' 'overlapping relay root was configured' >&2; exit 1
fi
cmp -s "$task/config" "$task/original" || { printf '%s\n' 'overlapping configuration was modified' >&2; exit 1; }
printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\n' "$task/missing-build-root" > "$task/config"
cp "$task/config" "$task/original"
if "$runner" configure-macos-vm --config "$task/config" --base "$task/base" --work-root "$task/work" --min-free-gib 1 > "$task/log" 2>&1; then
  printf '%s\n' 'missing relay root was configured' >&2; exit 1
fi
cmp -s "$task/config" "$task/original" || { printf '%s\n' 'missing-root configuration was modified' >&2; exit 1; }
printf 'schema_version=1\nserver=ckgit@example.invalid\nname=vm-test\nci_build_root=%s\n' "$task/build" > "$task/config"
"$runner" configure-macos-vm --config "$task/config" --base "$task/base" --work-root "$task/work" --min-free-gib 1 > "$task/log" 2>&1
case "$(cat "$task/log")" in *'can claim VM jobs'*) ;; *) cat "$task/log" >&2; exit 1;; esac
case "$(cat "$task/config")" in *"ci_macos_vm_base=$task/base"*) ;; *) printf '%s\n' 'VM configuration missing' >&2; exit 1;; esac
"$admin" runner macos-vm status --config "$task/config" > "$task/log" 2>&1 || { cat "$task/log" >&2; exit 1; }
case "$(cat "$task/log")" in *'Base components allocated (filesystem estimate): '*'Workspace-like directories: 0'*'Other entries: 0'*) ;; *) cat "$task/log" >&2; exit 1;; esac
python3 - "$task/base" "$task/log" <<'PY'
import os, pathlib, sys
base, log = sys.argv[1:]
expected = sum(os.stat(os.path.join(base, name)).st_blocks * 512 for name in
               ('Disk.img', 'AuxiliaryStorage', 'MachineIdentifier', 'HardwareModel'))
if f'Base components allocated (filesystem estimate): {expected} bytes' not in pathlib.Path(log).read_text():
    raise SystemExit('allocated-size report does not match the filesystem')
PY
printf 'leave intact' > "$task/work/unknown"
"$admin" runner macos-vm status --config "$task/config" > "$task/log" 2>&1 || { cat "$task/log" >&2; exit 1; }
case "$(cat "$task/log")" in *'Other entries: 1'*) ;; *) cat "$task/log" >&2; exit 1;; esac
[ "$(cat "$task/work/unknown")" = 'leave intact' ] || { printf '%s\n' 'status modified unknown data' >&2; exit 1; }
/bin/rm "$task/work/unknown"
sed 's/ci_macos_vm_min_free_gib=1/ci_macos_vm_min_free_gib=65536/' "$task/config" > "$task/low-config"
if "$admin" runner macos-vm status --config "$task/low-config" > "$task/log" 2>&1; then
  printf '%s\n' 'insufficient capacity was reported as sufficient' >&2; exit 1
fi
case "$(cat "$task/log")" in *'Available: '*'Capacity: insufficient'*) ;; *) cat "$task/log" >&2; exit 1;; esac
/bin/rm "$task/original"
printf '%s\n' 'VM runner startup guards passed'
