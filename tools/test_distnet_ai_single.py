#!/usr/bin/env python3
"""Single-guest verification of the NexOS distributed compute network -- AI task.

Works around the broken two-VM socket-tunnel by running ONE QEMU SLIRP guest
as the COMPUTE node and a Python HOST SCHEDULER on the real machine:

    host (scheduler):  TASK 1 ai ... -> guest:5456 (via hostfwd)
    guest (compute):   distnet compute  (runs the new exec_task "ai" branch)

QEMU user-net forwards a host UDP port to the guest's TASK port so the host can
push a task in, and the guest's RESULT (sent to the SLIRP gateway 10.0.2.2)
is NATed back to the host's 5457 socket.

Expected RESULT (no /boot/model.gguf on the image, so the AI engine
falls back to its built-in Markov engine -- still a successful inference):
    RESULT 1 ok what is 2 plus 2............

This verifies the genuinely new code: exec_task's hand-rolled "ai" parser
(preserves the multi-word prompt) and kern_ai_ask() driving the real engine.

Usage:  python3 test_distnet_ai_single.py [path/to/os_textboot.img]
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
MON_PORT = 4510
SER_LOG = "build/serial_ai_single.log"
# The image has no /boot/model.gguf, so ai_init() falls back to the built-in
# Markov engine (still a SUCCESSFUL inference).  The compute node therefore
# returns "RESULT 1 ok <generated text>" -- not "err no_model".  "err no_model"
# would only appear if ai_init() hard-failed (e.g. kmalloc OOM).
EXPECT_PREFIX = b"RESULT 1 ok"
PROMPT = "what is 2 plus 2"


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


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    if os.path.exists(SER_LOG):
        open(SER_LOG, "w").close()

    proc = subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % IMG,
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg,tb-size=16",
        "-net", "nic,model=ne2k_isa",
        "-net", "user,hostfwd=udp::54560-:5456",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-chardev", "file,id=ser,path=%s" % SER_LOG,
        "-serial", "chardev:ser",
    ], stdout=open("build/qemu_ai_single.err", "wb"),
       stderr=subprocess.STDOUT)

    ok = False
    s1 = s2 = None
    try:
        mon = wait_mon(MON_PORT)
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[test] guest: distnet compute")
        type_line(mon, "distnet compute")
        time.sleep(1.0)

        # Host scheduler sockets.  No discovery needed: the compute node's
        # TASK handler is bound unconditionally, so we push a TASK straight in.
        # s1 sends the TASK (through the hostfwd); s2 owns 5457 because the
        # compute node hard-codes DN_RESULT_PORT (5457) as the RESULT dst and
        # SLIRP NATs guest->gateway:5457 back to the host's 5457 socket.
        s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s1.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s2.bind(("0.0.0.0", 5457))
        s1.settimeout(1.0)
        s2.settimeout(1.0)

        # Dispatch the AI task directly (skip QUERY/BEACON -- the compute node
        # services TASK_PORT regardless of discovery).
        task = ("TASK 1 ai %s" % PROMPT).encode()
        print("[test] host: TASK -> guest:5456 (via hostfwd 54560) : %r" % task)
        s1.sendto(task, ("127.0.0.1", 54560))
        deadline = time.time() + 15.0
        while time.time() < deadline:
            try:
                data, _ = s2.recvfrom(1024)
                print("[test] host: RESULT <- %r" % data)
                if data.startswith(EXPECT_PREFIX) and PROMPT.encode() in data:
                    ok = True
                    break
            except socket.timeout:
                pass

        print("\n--- guest serial tail ---")
        if os.path.exists(SER_LOG):
            lines = open(SER_LOG, "rb").read().decode("latin-1", "ignore").splitlines()
            print("\n".join(lines[-18:]))
    finally:
        for s in (s1, s2):
            try:
                if s:
                    s.close()
            except Exception:
                pass
        try:
            proc.terminate()
            proc.wait(timeout=3.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    print("\nEXPECTED: %s ... (prompt '%s' preserved)" % (EXPECT_PREFIX.decode(), PROMPT))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
