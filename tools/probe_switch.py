#!/usr/bin/env python3
"""Minimal probe: does `switch` from a clean shell reach 64-bit kmain64?"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os.img"
LOG = "build/probe_switch.log"
ERR = "build/probe_switch.err"
MON_PORT = 4475

def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("port %d never opened" % port)

def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""

def wait_for(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.3)
    return False

def send_key(mon, key, d=0.12):
    mon.sendall(("sendkey %s\n" % key).encode()); time.sleep(d)

def type_line(mon, s):
    for c in s:
        send_key(mon, c)
    send_key(mon, "ret", 0.4)

if __name__ == "__main__":
    for f in (LOG, ERR):
        if os.path.exists(f):
            os.remove(f)
    errf = open(ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "128M", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG, "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(MON_PORT)
        if not wait_for("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot)"); sys.exit(1)
        time.sleep(1.5)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.5)
            type_line(mon, "admin"); time.sleep(1.0)
            type_line(mon, "echo boot-ok")
            if wait_for("boot-ok", 15.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.0)
        print("[LOGIN] done; sending 'switch' from clean shell")
        type_line(mon, "switch")
        ok = wait_for("[K64-1] kmain64 entered", 40.0)
        print("64-bit kmain64 reached:", ok)
        print("last 20 serial lines:")
        print("\n".join(ser_text().splitlines()[-20:]))
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: q.wait(timeout=6)
        except Exception:
            q.terminate(); q.kill()
        errf.close()
    print("RESULT:", "PASS" if ok else "FAIL")
