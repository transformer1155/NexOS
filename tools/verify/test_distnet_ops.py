#!/usr/bin/env python3
"""Single-guest verification of the extended distnet compute tasks.

Two independent boots (compute and scheduler are MUTUALLY EXCLUSIVE roles in
the kernel: net_udp_bind() refuses to replace a bound port, so a guest that
already ran `distnet compute` can never re-bind 5455 as a scheduler).

Boot 1 -- kernel COMPUTE side:
    Guest runs `distnet compute`; the host pushes raw TASK datagrams (via
    hostfwd) for every task type:
        sum 1 2 3 4 5    -> 15
        compute 7        -> 49
        neg 42           -> -42
        fib 12           -> 144
        prime 50         -> 15
        gcd 48 36        -> 12
    asserting the guest's exec_task() returns the right RESULT over the wire.

Boot 2 -- kernel SCHEDULER side (discovery + dispatch):
    Host-side compute peer answers QUERY/BEACON + executes tasks.  The guest
    acts as the scheduler:
        distnet nodes                 -> "nodes: total 1"
        distnet scheduler fib 12      -> RESULT 1 ok 144
        distnet scheduler prime 50    -> RESULT 1 ok 15
        distnet scheduler neg 42      -> RESULT 1 ok -42
        distnet scheduler gcd 48 36   -> RESULT 1 ok 12
        distnet scheduler             -> RESULT 1 ok 15 (default sum demo)
    asserting the guest serial shows each expected RESULT.  This exercises
    the generalized cmd_distnet parse (type + args) and the new
    distnet_nodes() discovery-only listing.

Usage:  python3 test_distnet_ops.py [path/to/os_textboot.img]
"""
import os
import sys
import socket
import time
import subprocess
import shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import distnet_host_peer as peer  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

QEMU = os.environ.get("QEMU_BIN") or shutil.which("qemu-system-x86_64")
if not QEMU:
    for cand in ("/d/qemu/qemu-system-x86_64.exe",
                 "/c/Program Files/qemu/qemu-system-x86_64.exe",
                 "D:/qemu/qemu-system-x86_64.exe"):
        if os.path.exists(cand):
            QEMU = cand
            break
if not QEMU:
    print("ERROR: qemu-system-x86_64 not found")
    sys.exit(2)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"

# Boot 1: direct TASK -> guest compute node.  (id, type, args, expected result)
TASKS = [
    ("1", "sum",     "1 2 3 4 5", "15"),
    ("2", "compute", "7",         "49"),
    ("3", "neg",     "42",        "-42"),
    ("4", "fib",     "12",        "144"),
    ("5", "prime",   "50",        "15"),
    ("6", "gcd",     "48 36",     "12"),
]

# Boot 2: guest acts as scheduler against the host peer.
SCHED = [
    ("distnet scheduler fib 12",    "RESULT 1 ok 144"),
    ("distnet scheduler prime 50",  "RESULT 1 ok 15"),
    ("distnet scheduler neg 42",    "RESULT 1 ok -42"),
    ("distnet scheduler gcd 48 36", "RESULT 1 ok 12"),
    ("distnet scheduler",           "RESULT 1 ok 15"),
]


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', '?': 'shift-slash'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def wait_mon(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready: %d" % port)


def launch(mon_port, serial_log, hostfwd):
    if os.path.exists(serial_log):
        open(serial_log, "w").close()
    errf = open(serial_log + ".err", "wb")
    proc = subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % IMG,
        "-m", "64M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg,tb-size=16",
        "-net", "nic,model=ne2k_isa",
        "-net", "user" + hostfwd,
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % mon_port,
        "-chardev", "file,id=ser,path=%s" % serial_log,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    mon = wait_mon(mon_port)
    time.sleep(8.0)
    type_line(mon, "root")
    type_line(mon, "admin")
    time.sleep(1.0)
    return proc, mon


def read_log(path):
    if not os.path.exists(path):
        return ""
    return open(path, "rb").read().decode("latin-1", "ignore")


def kill(proc):
    try:
        proc.terminate()
        proc.wait(timeout=3.0)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def boot1():
    """Guest = compute node; host pushes every task type directly."""
    print("=" * 60)
    print("[Boot 1] kernel compute node -- direct TASK dispatch")
    mon_port = 4521
    ser = "build/serial_distnet_ops_c.log"
    proc, mon = launch(mon_port, ser, ",hostfwd=udp::54560-:5456")

    ok = False
    s1 = s2 = None
    try:
        type_line(mon, "distnet compute")
        deadline = time.time() + 20.0
        while time.time() < deadline:
            if "compute node online" in read_log(ser):
                break
            time.sleep(0.3)

        s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s1.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s2.bind(("0.0.0.0", 5457))
        s1.settimeout(1.0)
        s2.settimeout(1.0)

        pending = {tid: exp for tid, _, _, exp in TASKS}
        for tid, typ, args, exp in TASKS:
            task = ("TASK %s %s %s" % (tid, typ, args)).encode()
            s1.sendto(task, ("127.0.0.1", 54560))
            print("[test] host: TASK -> guest: %r" % task)

        deadline = time.time() + 20.0
        while pending and time.time() < deadline:
            try:
                data, _ = s2.recvfrom(1024)
                print("[test] host: RESULT <- %r" % data)
                for tid, exp in list(pending.items()):
                    if data.startswith(("RESULT %s ok %s" % (tid, exp)).encode()):
                        del pending[tid]
                        break
            except socket.timeout:
                pass
        ok = not pending
        if pending:
            print("[test] FAIL: no result for ids: %s" % sorted(pending))
    finally:
        for s in (s1, s2):
            try:
                if s:
                    s.close()
            except Exception:
                pass
        kill(proc)
    print("[Boot 1] RESULT:", "PASS" if ok else "FAIL")
    return ok


def boot2():
    """Guest = scheduler; host peer answers beacons + tasks."""
    print("=" * 60)
    print("[Boot 2] kernel scheduler -- discovery + dispatch vs host peer")
    mon_port = 4522
    ser = "build/serial_distnet_ops_s.log"
    stop = peer.start_in_thread()
    time.sleep(0.5)
    proc, mon = launch(mon_port, ser, "")

    ok_nodes = False
    ok_sched = True
    try:
        print("[test] guest: distnet nodes")
        type_line(mon, "distnet nodes")
        deadline = time.time() + 25.0
        while time.time() < deadline:
            if "nodes: total 1" in read_log(ser):
                ok_nodes = True
                break
            time.sleep(0.4)
        if not ok_nodes:
            print("[test] FAIL: distnet nodes did not list the peer")

        for cmd, expect in SCHED:
            print("[test] guest: %s  (expect %s)" % (cmd, expect))
            type_line(mon, cmd)
            # TCG + 64M is slow: the QUERY/ARP discovery handshake can need
            # many QUERY retries (SLIRP drops the first broadcast), so give
            # each dispatch a generous window.
            deadline = time.time() + 45.0
            hit = False
            while time.time() < deadline:
                if expect in read_log(ser):
                    hit = True
                    break
                time.sleep(0.4)
            if not hit:
                ok_sched = False
                print("[test] FAIL: %s did not produce %s" % (cmd, expect))
            time.sleep(0.5)

        print("\n--- guest serial tail (boot 2) ---")
        lines = read_log(ser).splitlines()
        print("\n".join(lines[-40:]))
    finally:
        stop.set()
        kill(proc)
    print("[Boot 2] nodes list:", "PASS" if ok_nodes else "FAIL")
    print("[Boot 2] scheduler ops:", "PASS" if ok_sched else "FAIL")
    return ok_nodes and ok_sched


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    r1 = boot1()
    r2 = boot2()
    print("=" * 60)
    print("Boot1 (kernel compute):", "PASS" if r1 else "FAIL")
    print("Boot2 (kernel scheduler):", "PASS" if r2 else "FAIL")
    print("RESULT:", "PASS" if (r1 and r2) else "FAIL")
    return 0 if (r1 and r2) else 1


if __name__ == "__main__":
    sys.exit(main())
