#!/usr/bin/env python3
"""macOS diagnostic: ancestry loss versus UID discovery, not a CI supervisor.

Run with python3 -B and TMPDIR set to a task directory on a mounted approved
scratch volume. No compilation, account changes, or UID-wide signals occur.
The orphan uses an inherited private pipe solely for deterministic test cleanup;
a real untrusted job can close that pipe, so it is not a production mechanism.
"""
import ctypes, json, os, select, signal, sys, time

if sys.platform != 'darwin':
    raise SystemExit('This diagnostic requires macOS')
roots = ('/Volumes/S8T', '/Volumes/PRO-BLADE')
tmp = os.path.realpath(os.environ.get('TMPDIR', ''))
if not any(os.path.ismount(r) and tmp.startswith(r + '/tmp/') for r in roots):
    raise SystemExit('Set TMPDIR to an approved mounted-volume task directory')
if not os.path.isdir(tmp):
    raise SystemExit('TMPDIR must already exist')

# Only the root child (still owned and unreaped) is signaled. The orphan exits
# on its private control pipe's EOF; every child also has a 10-second alarm.
libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
libproc.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
libproc.proc_listpids.restype = ctypes.c_int

def pids(kind, value):
    capacity = 4096
    for _ in range(8):
        buf = (ctypes.c_int * capacity)()
        ctypes.set_errno(0)
        n = libproc.proc_listpids(kind, value, buf, ctypes.sizeof(buf))
        if n < 0 or (n == 0 and ctypes.get_errno()):
            raise RuntimeError(('proc_listpids', kind, n, ctypes.get_errno()))
        if n < ctypes.sizeof(buf):
            return {p for p in buf[:n // ctypes.sizeof(ctypes.c_int)] if p}
        capacity *= 2
    raise RuntimeError('process table kept growing')

def state():
    return dict(pid=os.getpid(), ppid=os.getppid(), pgid=os.getpgrp(),
                sid=os.getsid(0), uid=os.getuid(), euid=os.geteuid())

if len(sys.argv) > 1 and sys.argv[1] == '--orphan':
    signal.alarm(10)
    out, control = map(int, sys.argv[2:])
    deadline = time.monotonic() + 3
    while os.getppid() != 1 and time.monotonic() < deadline:
        time.sleep(.001)
    os.write(out, (json.dumps(state()) + '\n').encode())
    os.close(out)
    os.read(control, 1)
    os._exit(0)

def trial():
    report_r, report_w = os.pipe()
    control_r, control_w = os.pipe()
    gate_r, gate_w = os.pipe()
    root = os.fork()
    if root == 0:
        signal.alarm(10)
        os.close(report_r); os.close(control_w); os.close(gate_w)
        os.setsid()
        os.read(gate_r, 1)
        middle = os.fork()
        if middle == 0:
            os.setsid()
            orphan = os.fork()
            if orphan:
                os._exit(0)
            os.set_inheritable(report_w, True)
            os.set_inheritable(control_r, True)
            os.execv(sys.executable, [sys.executable, '-B', __file__, '--orphan', str(report_w), str(control_r)])
        os.waitpid(middle, 0)
        os.close(report_w)
        os.read(control_r, 1)
        os._exit(0)
    os.close(report_w); os.close(control_r); os.close(gate_r)
    reaped = False
    queue = select.kqueue()
    try:
        queue.control([select.kevent(root, filter=select.KQ_FILTER_PROC,
            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
            fflags=select.KQ_NOTE_FORK | select.KQ_NOTE_EXIT)], 0, 0)
        os.write(gate_w, b'1'); os.close(gate_w); gate_w = -1
        if not select.select([report_r], [], [], 5)[0]:
            raise RuntimeError('orphan did not report')
        data = os.read(report_r, 4096)
        orphan = json.loads(data)
        assert orphan['ppid'] == 1, orphan
        assert orphan['pgid'] != root and orphan['sid'] != root, orphan
        assert orphan['uid'] == os.getuid() and orphan['euid'] == os.geteuid()
        # Hold orphan alive on its private pipe while observing and cancelling.
        queue.control([select.kevent(orphan['pid'], filter=select.KQ_FILTER_PROC,
            flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
            fflags=select.KQ_NOTE_EXIT)], 0, 0)
        children = pids(6, root)  # PROC_PPID_ONLY
        by_uid = pids(4, os.geteuid())  # PROC_UID_ONLY
        assert orphan['pid'] not in children
        assert orphan['pid'] in by_uid
        events = queue.control(None, 8, 0)
        os.killpg(root, signal.SIGKILL)
        os.waitpid(root, 0); reaped = True
        os.kill(orphan['pid'], 0)
        assert orphan['pid'] in pids(4, os.geteuid())
        os.close(control_w); control_w = -1
        exited = False
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not exited:
            for event in queue.control(None, 8, .1):
                if event.ident == orphan['pid'] and event.fflags & select.KQ_NOTE_EXIT:
                    exited = True
        assert exited, 'orphan failed to exit on pipe EOF'
        return dict(root=root, orphan=orphan, root_children=sorted(children),
                    uid_found_orphan=True, survived_group_kill=True,
                    orphan_exit_observed=True,
                    fork_events=[dict(pid=e.ident, flags=e.fflags, data=e.data) for e in events])
    finally:
        for fd in (report_r, control_w, gate_w):
            if fd >= 0: os.close(fd)
        if not reaped:
            # Direct root PID cannot be reused until we reap it.
            os.kill(root, signal.SIGKILL)
            os.waitpid(root, 0)
        queue.close()

started = time.time()
results = [trial() for _ in range(25)]
print(json.dumps(dict(start_epoch=started, elapsed_seconds=time.time()-started,
                     trials=results, passed=len(results)), indent=2))
