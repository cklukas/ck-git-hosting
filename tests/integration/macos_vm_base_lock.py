#!/usr/bin/env python3
"""Opt-in base-lock check; requires a stopped, operator-prepared macOS VM base."""
import fcntl
import os
import subprocess
import sys
import tempfile

if len(sys.argv) != 5:
    sys.exit("usage: macos_vm_base_lock.py HELPER OFFLINE-PROBE BASE TASK-DIRECTORY")
helper, probe, base, task = sys.argv[1:]
root = tempfile.mkdtemp(prefix="base-lock-", dir=task)
os.chmod(root, 0o700)
fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
try:
    fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
    result = subprocess.run([helper, "setup", base], capture_output=True, text=True, timeout=15)
    if result.returncode == 0 or "in use" not in result.stderr:
        raise RuntimeError("setup did not reject a base held for cloning: " + result.stderr)
    fcntl.flock(fd, fcntl.LOCK_UN)
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    result = subprocess.run([probe, base, root, helper], capture_output=True, text=True, timeout=15)
    if result.returncode == 0 or "in use by setup" not in result.stderr:
        raise RuntimeError("cloning did not reject a base held for setup: " + result.stderr)
    if os.listdir(root):
        raise RuntimeError("rejected clone left unexpected data in " + root)
    print("Base setup and cloning exclude each other; no VM was started.")
finally:
    os.close(fd)
    # Only remove the task-owned directory when it is empty.
    os.rmdir(root)
