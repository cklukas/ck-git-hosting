# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Sourced by the integration tests: stops a background process they started,
# without ever waiting for it unbounded.
#
# Why bounded: these scripts trap TERM (to clean up), and a forked background
# job carries that trap until it has reset it and started its program. A
# SIGTERM arriving in that short window -- a check failing right after the
# job was started, so the EXIT trap kills it at once -- is taken as a pending
# trap and lost when the program starts; the program then runs on, and a
# plain `wait` would hang the test. Seen once with `ckdocs serve` in
# docs_site.sh and reproduced in about 1 of 150 such starts.

# True while process $1 is running: not gone, and not an exited child that
# only waits to be reaped (`kill -0` succeeds for those too).
process_running() {
  process_state=$(ps -o stat= -p "$1" 2>/dev/null) || return 1
  case "$process_state" in
    Z* | '') return 1 ;;
  esac
  return 0
}

# stop_process PID [SIGNAL]: sends SIGNAL (default TERM), waits up to 10 s,
# then SIGKILLs a process that is still running and says so; always reaps it.
# Returns 1 when SIGKILL was needed, 0 otherwise; an empty PID is a no-op.
stop_process() {
  [ -n "${1:-}" ] || return 0
  kill "-${2:-TERM}" "$1" 2>/dev/null || true
  stop_attempt=0
  while process_running "$1" && [ "$stop_attempt" -lt 100 ]; do
    stop_attempt=$((stop_attempt + 1))
    sleep .1
  done
  stop_status=0
  if process_running "$1"; then
    echo "process $1 did not stop within 10 s of SIG${2:-TERM}; killing it" >&2
    if [ -r "/proc/$1/status" ]; then
      grep -E '^(State|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt):' "/proc/$1/status" >&2 || true
    fi
    kill -KILL "$1" 2>/dev/null || true
    stop_status=1
  fi
  wait "$1" 2>/dev/null || true
  return "$stop_status"
}
