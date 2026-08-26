#!/usr/bin/env python3
"""
Runtime verification for the 64-bit `plugin` command (item 3 of the GUI
improvements).  The 64-bit kernel (kernel64.cpp) runs a temporary self-test
at boot (gated by [K64-VERIFY] markers) that drives `plugin list` and
`plugin toggle nexos.knowledge` through the real run_command() dispatcher
and dumps the captured output to the serial port.  This script boots the
freshly built os.img headlessly and inspects that serial dump.

Uses Windows QEMU (D:\\qemu), tcg accelerator (WHPX is unavailable).
"""
import os, sys, time, socket, subprocess, re

IMG   = "build/os.img"
LOG   = "build/serial_plugin.log"
MON_PORT = 4451
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"

def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""

def wait_for(needle, timeout=90.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.3)
    return False

def extract(start, end):
    t = ser_text()
    s = t.find(start)
    e = t.find(end, s + 1) if s >= 0 else -1
    if s < 0 or e < 0:
        return ""
    return t[s + len(start):e]

def main():
    if not os.path.exists(QEMU):
        print("QEMU not found: %s" % QEMU); sys.exit(2)
    if not os.path.exists(IMG):
        print("IMG not found: %s" % IMG); sys.exit(2)
    if os.path.exists(LOG):
        os.remove(LOG)

    errf = open("build/plugin_err.log", "wb")
    q = subprocess.Popen([
        QEMU, "-m", "512M", "-accel", "tcg", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    try:
        ok = True
        # 1) 64-bit kernel entered
        if not wait_for("[K64-1] kmain64 entered", timeout=60.0):
            print("FAIL: 64-bit kernel never entered"); ok = False
        else:
            print("OK: 64-bit kernel entered ([K64-1])")

        # 2) self-test ran
        if not wait_for("[K64-VERIFY] plugin command self-test end", timeout=60.0):
            print("FAIL: plugin self-test markers not seen")
            ok = False
        else:
            print("OK: plugin self-test markers seen")

        # 3) catalogue content
        cat = extract("[K64-VERIFY-LIST]\n", "\n[K64-VERIFY-LIST-END]")
        if "NexOS plugin catalogue" in cat:
            rows = [l for l in cat.splitlines() if "nexos." in l]
            print("OK: `plugin list` printed catalogue (%d plugin rows)" % len(rows))
            if len(rows) != 23:
                print("WARN: expected 23 plugin rows, got %d" % len(rows))
        else:
            print("FAIL: `plugin list` produced no catalogue"); ok = False

        # 4) toggle content
        tog = extract("[K64-VERIFY-TOGGLE]\n", "\n[K64-VERIFY-TOGGLE-END]")
        if "nexos.knowledge" in tog and ("loaded" in tog or "unloaded" in tog):
            print("OK: `plugin toggle nexos.knowledge` -> " + tog.strip())
        else:
            print("FAIL: `plugin toggle` produced no result"); ok = False

        # 5) crash markers
        t = ser_text()
        crash = ("PANIC" in t or "triple fault" in t or "Exception #" in t
                 or "isr_stub" in t.lower() and "fault" in t.lower())
        if crash:
            print("FAIL: crash markers in serial"); ok = False
        else:
            print("OK: no crash markers")

        print("RESULT: " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception: pass

if __name__ == "__main__":
    sys.exit(main())
