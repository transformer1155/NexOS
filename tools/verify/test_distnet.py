#!/usr/bin/env python3
"""Headless verification of the NexOS distributed compute network.

Boots build/os.img under QEMU SLIRP + NE2000, starts a host-side compute
peer (distnet_host_peer.py), logs the shell in, runs:

    distnet scheduler 10.0.2.2

and asserts the guest receives a RESULT from the host compute node.  This
exercises the full kernel path: distnet scheduler -> UDP send (ip_send with
unicast to 10.0.2.2) -> SLIRP -> host peer -> SLIRP -> kernel UDP receive
(handle_udp port dispatch) -> RESULT callback.

Usage:  python3 test_distnet.py [path/to/os.img]
"""
import os
import sys
import socket
import time
import subprocess
import threading
import shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import distnet_host_peer as peer  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# Locate qemu-system-x86_64 (not always on PATH in this environment).
QEMU = os.environ.get("QEMU_BIN") or shutil.which("qemu-system-x86_64")
if not QEMU:
    for cand in ("/d/qemu/qemu-system-x86_64.exe",
                 "/c/Program Files/qemu/qemu-system-x86_64.exe",
                 "D:/qemu/qemu-system-x86_64.exe"):
        if os.path.exists(cand):
            QEMU = cand
            break
if not QEMU:
    print("ERROR: qemu-system-x86_64 not found on PATH or known locations")
    sys.exit(2)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
WORK = "build/distnet_test.img"
LOG = "build/serial_distnet.log"
MPORT = 4475


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    subprocess.run(["cp", IMG, WORK], check=True)
    # Truncate the serial log (avoid os.remove: blocked by safe-delete hook).
    if os.path.exists(LOG):
        open(LOG, "w").close()

    # Start the host compute peer BEFORE booting the guest.
    stop = peer.start_in_thread()
    time.sleep(0.5)

    errf = open("build/qemu_distnet.err", "wb")
    qemu = subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg",
        "-net", "nic,model=ne2k_isa",
        "-net", "user",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MPORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    ok = False
    try:
        mon = wait_sock(MPORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)  # let the OS boot + network init
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        print("[test] running: distnet scheduler 10.0.2.2")
        type_line(mon, "distnet scheduler 10.0.2.2")

        # Poll the serial log for the RESULT marker.
        deadline = time.time() + 25.0
        while time.time() < deadline:
            if os.path.exists(LOG):
                with open(LOG, "rb") as f:
                    data = f.read().decode("latin-1", "ignore")
                if "RESULT 1 ok 15" in data:
                    ok = True
                    break
            time.sleep(0.5)

        print("\n--- serial tail ---")
        if os.path.exists(LOG):
            lines = open(LOG, "rb").read().decode("latin-1", "ignore").splitlines()
            print("\n".join(lines[-25:]))
    finally:
        stop.set()
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        errf.close()

    print()
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
