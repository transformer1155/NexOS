#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_skill.py  -  Headless verification of the P4 AI skill system.

Boots NexOS (BIOS image) with the writable MKFS data disk attached as the
second IDE drive, formats it, then exercises the `create_file` skill through
the natural-language `agent` command and proves the file really landed:

    mkfs
    agent skills
    agent run create file hello.txt content=HelloNexOS
    cat hello.txt

Verdict comes from the serial log.  English trigger phrases are used because
QEMU `sendkey` cannot type CJK; the skill's Chinese keywords are still wired
and exercised by hand on real hardware.

Usage:
    python3 tools/test_skill.py
"""
import os
import sys
import socket
import time
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

BASE = "build/os.img"
DATA = "build/data.vhd"
WORK = "build/skill_test.img"
LOG = "build/serial_skill.log"
ERR = "build/qemu_skill.err"
PORT = 4461
MKFS_PY = "tools/make_data_vhd.py"
SFS_ALT_LBA = 3368


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def send_key(mon, key, delay=0.10):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.08):
    # Robust ASCII keymap for QEMU sendkey.  CJK is not typeable this way.
    keymap = {
        ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
        '-': 'minus', '_': 'shift-minus', '=': 'equal', ':': 'shift-semicolon',
    }
    for ch in s:
        if 'A' <= ch <= 'Z':
            key = "shift-%s" % ch.lower()
        else:
            key = keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.45)


def main():
    if not os.path.exists(BASE):
        print("ERROR: %s missing (run `make build/os.img` first)" % BASE)
        sys.exit(2)

    # 1. ensure the writable data disk exists
    if not os.path.exists(DATA):
        print("Creating data disk: %s ..." % DATA)
        subprocess.run([sys.executable, MKFS_PY, "8"], check=True)

    # 2. working copy of the boot image
    shutil.copyfile(BASE, WORK)

    for f in (LOG, ERR):
        if os.path.exists(f):
            os.remove(f)
    errf = open(ERR, "wb")

    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=%s,if=ide,index=0" % WORK,
        "-drive", "format=raw,file=%s,if=ide,index=1" % DATA,
        "-m", "128M",
        "-vga", "none",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    checks = {}
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        type_line(mon, "mkfs")                       # format the data disk
        time.sleep(1.0)
        type_line(mon, "agent skills")               # list skills
        time.sleep(0.8)
        type_line(mon, "agent run create file hello.txt content=HelloNexOS")
        time.sleep(1.0)
        type_line(mon, "cat hello.txt")              # read it back
        time.sleep(0.8)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            q.wait(timeout=3.0)
    errf.close()

    with open(LOG, "rb") as f:
        # The kernel emits UTF-8 (skill messages are Chinese); decoding as
        # latin-1 would make every CJK assertion below fail spuriously.
        txt = f.read().decode("utf-8", "ignore")

    # ---- verdict ----
    # `cat` output must be looked for *after* the last command echo, otherwise
    # the shell echoing "content=HelloNexOS" back would satisfy the check on
    # its own and the test would pass without the file ever existing.
    tail = txt.rsplit("cat hello.txt", 1)[-1]

    checks["skill listed"] = ("create_file" in txt)
    checks["skill dispatched"] = ("已创建文件 hello.txt" in txt)
    checks["byte count 10"] = ("(10 字节)" in txt)
    checks["file readable"] = ("HelloNexOS" in tail)

    print("=" * 60)
    print("P4 SKILL SYSTEM HEADLESS TEST")
    print("=" * 60)
    ok = True
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "FAIL", k))
        ok = ok and v
    print("\nRESULT:", "PASS" if ok else "FAIL")
    if not ok:
        print("--- serial tail ---")
        print("\n".join(txt.splitlines()[-25:]))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
