#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_dnspy_winapp.py - Verify NexOS's PE32 loader behaviour on a real
64-bit Windows binary (dnSpy.exe = PE32+, machine 0x8664).

This drives the `winapp` command, which routes .exe through win32.cpp's
real PE32 executor (NOT the legacy winloader detector used by `run`).
Expected outcome: win32_run() rejects the image at the i386 gate
(win32.cpp:1973) with "only 32-bit PE32 i386 images can execute" / -3,
and additionally would reject a managed CLR image.

Usage (WSL):
    python3 tools/test_dnspy_winapp.py
Requires dnSpy.exe to already be packed into sfs_files/ (see the inject step
in the task) and build/os.img rebuilt from it.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_dnspy.log"
PORT = 4459
MON_ERR = "build/qemu_dnspy.err"
SFS_LBA = 3368
WORK = "build/dnspy_test.img"


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def send_key(mon, key, delay=0.12):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.09):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.4)


def main():
    # Build a working copy that already contains dnSpy in SFS.
    if not os.path.exists(IMG):
        print("build/os.img missing; run `make build/os.img` first")
        sys.exit(2)
    shutil_copy = subprocess.run(
        ["cp", IMG, WORK], check=True)

    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            os.remove(f)
    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
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
        # inspect only (headers/sections/machine) - safe, no execution
        print("[SHELL] winapp /i dnspy.exe")
        type_line(mon, "winapp /i dnspy.exe")
        time.sleep(2.5)
        # actual run attempt - loader should reject at the i386 gate
        print("[SHELL] winapp dnspy.exe")
        type_line(mon, "winapp dnspy.exe")
        time.sleep(2.5)
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
        txt = f.read().decode("latin-1", "ignore")

    print("\n" + "=" * 60)
    print("dnSpy.exe via `winapp` (win32.cpp PE32 executor)")
    print("=" * 60)
    checks = {}
    checks["found_in_sfs"] = "File not found" not in txt
    checks["machine_reported"] = ("0x00008664" in txt or "0x8664" in txt) and "UNSUPPORTED" in txt
    checks["i386_gate"] = ("only 32-bit PE32 i386" in txt) or ("needs 32-bit i386" in txt)
    checks["return_neg3"] = "[X] Unsupported PE" in txt or "Unsupported PE" in txt
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))
    if "EXCEPTION" in txt:
        print("  WARNING: kernel EXCEPTION in serial log")

    # Echo the relevant kernel lines for the record.
    print("\n--- win32 loader output (serial) ---")
    for line in txt.splitlines():
        if any(t in line for t in ("winapp", "PE32 image", "Machine", "UNSUPPORTED",
                                    "only 32-bit", "needs 32-bit", "CLR", ".NET",
                                    "Unsupported PE", "[X]", "win32", "WIN32")):
            print("  " + line.strip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    print("(PASS here means: dnSpy was found in SFS but the loader correctly "
          "refused to execute it because it is a 64-bit PE32+ image.)")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
