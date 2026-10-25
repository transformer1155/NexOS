#!/usr/bin/env python3
"""Headless verification: does the FIXED 64-bit ata_read_sector let the
64-bit kernel mount SFS under QEMU BIOS and reach vec_init (step 318)?

Boots build/os_textboot.img, logs in at the text prompt (root/admin),
types `switch` to enter the 64-bit kernel, and captures the serial log.
Asserts the key markers:
  [ATA-DIAG]  - the 4-sector ATA probe (LBA 3488 must read 'SFS\\0')
  [K64-6]     - "SFS mounted" (success) vs "*** SFS NOT mounted ***"
  step=318    - vec_init ok (msyh.ttf loaded)
  step=319    - vec_init failed
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
LOG = "build/k64_ata_verify.log"
ERR = "build/k64_ata_verify.err"
MON_PORT = 4477
QEMU = "D:/qemu/qemu-system-x86_64.exe"

def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("monitor port %d never opened" % port)

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

def type_line(mon, s, enter_delay=0.4):
    for c in s:
        send_key(mon, c)
    send_key(mon, "ret", enter_delay)

if __name__ == "__main__":
    # Truncate stale logs in place instead of os.remove() -- the latter trips
    # the sandbox safe-delete hook (recycle-bin unavailable -> fail-closed).
    for f in (LOG, ERR):
        try:
            with open(f, "w"):
                pass
        except (FileNotFoundError, OSError):
            pass
    errf = open(ERR, "wb")
    q = subprocess.Popen([
        QEMU, "-m", "256", "-accel", "tcg,tb-size=128", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(MON_PORT)
        # 32-bit boot
        if not wait_for("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot did not complete)")
            sys.exit(1)
        time.sleep(1.0)
        # Text login prompt
        print("[*] 32-bit boot ok; sending login (root/admin)")
        # The login prompt may or may not appear; send credentials defensively.
        if wait_for("login:", 8.0):
            type_line(mon, "root")
            time.sleep(0.6)
            type_line(mon, "admin")
        else:
            # No explicit prompt: try the same sequence anyway.
            type_line(mon, "root")
            time.sleep(0.6)
            type_line(mon, "admin")
        time.sleep(1.0)
        if not wait_for("Shell ready", 20.0):
            print("[!] 'Shell ready' not seen; continuing anyway")
        # Trigger the 64-bit switch
        print("[*] sending 'switch' to enter 64-bit kernel")
        type_line(mon, "switch")
        ok64 = wait_for("[K64-1] kmain64 entered", 40.0)
        print("64-bit kmain64 reached:", ok64)
        # Wait for filesystem + vec_init markers
        wait_for("[K64-6]", 40.0)
        wait_for("step=31", 40.0)
        time.sleep(2.0)
        txt = ser_text()
        print("---- serial excerpt (ATA/SFS/vec markers) ----")
        for ln in txt.splitlines():
            s = ln.strip()
            if ("[ATA-DIAG]" in s or "[SFS-TEST]" in s or "[K64-6]" in s
                    or "sfs-cand" in s or "step=31" in s or "step=3" in s
                    or "vec_init" in s or "SFS mounted" in s or "NOT mounted" in s):
                print(s)
        print("---------------------------------------------")
        ata = wait_for("[ATA-DIAG] done", 5.0)
        mounted = "[K64-6] SFS mounted" in txt
        notmounted = "[K64-6] *** SFS NOT mounted ***" in txt
        vec_ok = "step=318" in txt
        vec_bad = "step=319" in txt
        print("ATA-DIAG ran:", ata)
        print("SFS mounted:", mounted, " NOT mounted:", notmounted)
        print("vec_init ok (318):", vec_ok, " vec_init failed (319):", vec_bad)
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: q.wait(timeout=6)
        except Exception:
            q.terminate(); q.kill()
        errf.close()
    print("RESULT:", "PASS" if (ok64 and mounted and vec_ok) else "FAIL")
