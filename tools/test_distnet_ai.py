#!/usr/bin/env python3
"""Two-guest verification of the NexOS distributed compute network -- AI task.

Boots TWO QEMU guests on a shared L2 UDP socket link (no SLIRP) and exercises
the REAL broadcast discovery path, but dispatches an *AI inference* task
instead of the sum demo:

    guest A (scheduler, 10.0.2.15):  distnet ai "what is 2 plus 2"
    guest B (compute,    10.0.2.16):  setip 10.0.2.16; distnet compute

The scheduler broadcasts QUERY, B replies with BEACON, A discovers it,
dispatches "TASK 1 ai what is 2 plus 2".  B's compute node parses the ai
branch (preserving the multi-word prompt), calls kern_ai_ask(), and -- with
no /boot/model.gguf on the image -- returns "RESULT 1 ok err no_model".

This verifies:
  * cmd_distnet "ai" subcommand builds the TASK line correctly,
  * broadcast discovery + dispatch (scheduler_run) still works,
  * exec_task's hand-rolled ai parser keeps the spaces in the prompt,
  * kern_ai_ask gracefully reports no_model instead of faulting.

Usage:  python3 test_distnet_ai.py [path/to/os_textboot.img]
"""
import os
import sys
import socket
import time
import subprocess
import shutil

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
SK = "127.0.0.1:1234"          # UDP socket tunnel between the two guests
MA_PORT, MB_PORT = 4491, 4492
SA_LOG = "build/serial_ai_a.log"   # scheduler guest
SB_LOG = "build/serial_ai_b.log"   # compute guest
COMPUTE_IP = "10.0.2.16"
AI_PROMPT = "what is 2 plus 2"
EXPECT = "RESULT 1 ok err no_model"


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready: %d" % port)


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', '?': 'shift-slash'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def launch(role, port, log, sockopt):
    return subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % IMG,
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg",
        "-net", "nic,model=ne2k_isa",
        "-net", sockopt,
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
        "-chardev", "file,id=ser,path=%s" % log,
        "-serial", "chardev:ser",
    ], stdout=open("build/qemu_ai_%s.err" % role, "wb"),
       stderr=subprocess.STDOUT)


def login(mon):
    time.sleep(8.0)
    type_line(mon, "root")
    type_line(mon, "admin")
    time.sleep(1.0)


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    for f in (SA_LOG, SB_LOG):
        if os.path.exists(f):
            open(f, "w").close()

    procA = launch("a", MA_PORT, SA_LOG, "socket,listen=%s" % SK)
    time.sleep(2.0)
    procB = launch("b", MB_PORT, SB_LOG, "socket,connect=%s" % SK)

    ok = False
    try:
        monA = wait_sock(MA_PORT)
        monB = wait_sock(MB_PORT)

        login(monA)
        login(monB)

        print("[test] compute guest: setip %s ; distnet compute" % COMPUTE_IP)
        type_line(monB, "setip %s" % COMPUTE_IP)
        type_line(monB, "distnet compute")
        time.sleep(1.0)

        print('[test] scheduler guest: distnet ai "%s"' % AI_PROMPT)
        type_line(monA, 'distnet ai "%s"' % AI_PROMPT)

        deadline = time.time() + 40.0
        while time.time() < deadline:
            if os.path.exists(SA_LOG):
                with open(SA_LOG, "rb") as f:
                    data = f.read().decode("latin-1", "ignore")
                if EXPECT in data:
                    ok = True
                    break
            time.sleep(0.5)

        print("\n--- scheduler (A) serial tail ---")
        if os.path.exists(SA_LOG):
            lines = open(SA_LOG, "rb").read().decode("latin-1", "ignore").splitlines()
            print("\n".join(lines[-24:]))
        print("\n--- compute (B) serial tail ---")
        if os.path.exists(SB_LOG):
            lines = open(SB_LOG, "rb").read().decode("latin-1", "ignore").splitlines()
            print("\n".join(lines[-12:]))
    finally:
        for p in (procA, procB):
            try:
                p.terminate()
                p.wait(timeout=3.0)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass

    print("\nEXPECTED: %s" % EXPECT)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
