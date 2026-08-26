#!/usr/bin/env python3
"""Probe: does the 64-bit ggml adapter self-test survive with a SMALL blob?"""
import os, sys, socket, time, subprocess, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
QEMU = os.environ.get("QEMU_BIN") or shutil.which("qemu-system-x86_64")
if not QEMU:
    for cand in ("/d/qemu/qemu-system-x86_64.exe", "D:/qemu/qemu-system-x86_64.exe"):
        if os.path.exists(cand):
            QEMU = cand
            break
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/probe_small.img"
MON_PORT = 4561
SER_LOG = "build/serial_probe_small.log"


def wait_mon(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash', '-': 'minus', '_': 'shift-minus', '?': 'shift-slash'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def read_log():
    if not os.path.exists(SER_LOG):
        return ""
    return open(SER_LOG, "rb").read().decode("latin-1", "ignore")


def main():
    if not os.path.exists(IMG):
        print("ERROR: no image")
        return 1
    if os.path.exists(SER_LOG):
        open(SER_LOG, "w").close()
    errf = open("build/qemu_probe_small.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-drive", "format=raw,file=%s" % IMG,
        "-m", "1536M", "-vga", "std", "-display", "none",
        "-accel", "tcg,tb-size=8",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-chardev", "file,id=ser,path=%s" % SER_LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_mon(MON_PORT)
        deadline = time.time() + 60.0
        while time.time() < deadline:
            if "login:" in read_log():
                break
            time.sleep(0.5)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[test] ask64 probe")
        type_line(mon, "ask64 hello")

        # Wait for kernel64's OWN login prompt stage (2nd PERM marker),
        # then blind-type credentials in a retry loop until login succeeds
        # (the PS/2 queue only buffers a few bytes, so timing matters).
        deadline = time.time() + 180.0
        while time.time() < deadline:
            if read_log().count("[PERM] Y/N prompt engine armed") >= 2:
                break
            time.sleep(1.0)
        print("[test] kernel64 reached login stage; retrying credentials")
        deadline = time.time() + 120.0
        while time.time() < deadline:
            if "[K64-LOGIN]" in read_log():
                break
            type_line(mon, "root")
            type_line(mon, "admin")
            type_line(mon, "")
            time.sleep(4.0)

        # wait for 64-bit markers
        deadline = time.time() + 240.0
        while time.time() < deadline:
            data = read_log()
            if "ADAPT] === done ===" in data or "M-FOR" in data:
                break
            if data.count("[ADAPT] disk: blob present") >= 1 and "disk parse" in data:
                break
            time.sleep(1.0)
        print("\n--- serial (ADAPT section) ---")
        for line in read_log().splitlines():
            if "ADAPT" in line or "K64" in line or "M-" in line or "LOGIN" in line:
                print(line)
    finally:
        try:
            qemu.terminate(); qemu.wait(timeout=3.0)
        except Exception:
            try: qemu.kill()
            except Exception: pass
        errf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
