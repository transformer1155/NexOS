#!/usr/bin/env python3
"""Headless verification of the in-kernel `distnet ai` SCHEDULER command.

Boots build/os_textboot.img under QEMU SLIRP + NE2000, starts the shared
host-side compute peer (distnet_host_peer.py), logs in, and runs:

    distnet ai what is 2 plus 2

asserting the guest (acting as scheduler) receives a RESULT whose text is the
prompt echoed back.  This exercises the scheduler-side new code:
  * cmd_distnet "ai" subcommand parsing (rest-of-line prompt),
  * distnet_scheduler_ai() building "TASK 1 ai <prompt>" (spaces preserved),
  * unicast discovery + dispatch via scheduler_run().

The host peer has no model, so its "ai" handler echoes the prompt verbatim --
which is exactly what we want to assert: the kernel shipped the multi-word
prompt across the wire intact.

Note: no shell quotes are typed.  The QEMU monitor `sendkey` has no key name
for '"', and cmd_distnet takes the whole rest of the line as the prompt.

Usage:  python3 test_distnet_ai_sched.py [path/to/os_textboot.img]
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
WORK = "build/distnet_ai_sched.img"
LOG = "build/serial_ai_sched.log"
MPORT = 4476
PROMPT = "what is 2 plus 2"


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
    if os.path.exists(LOG):
        open(LOG, "w").close()

    stop = peer.start_in_thread()
    time.sleep(0.5)

    errf = open("build/qemu_ai_sched.err", "wb")
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
    expect = "RESULT 1 ok %s" % PROMPT
    try:
        mon = wait_sock(MPORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)          # let the OS boot + network init
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        cmd = "distnet ai %s" % PROMPT
        print("[test] running: %s" % cmd)
        type_line(mon, cmd)

        deadline = time.time() + 30.0
        while time.time() < deadline:
            with open(LOG, "rb") as f:
                data = f.read().decode("latin-1", "ignore")
            if expect in data:
                ok = True
                break
            time.sleep(0.5)

        print("\n--- serial tail ---")
        lines = open(LOG, "rb").read().decode("latin-1", "ignore").splitlines()
        print("\n".join(lines[-22:]))
    finally:
        stop.set()
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        errf.close()

    print("\nEXPECTED: %s" % expect)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
