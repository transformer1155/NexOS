#!/usr/bin/env python3
"""
Boot smoke test for the NexOS BIOS raw image (build/os.img).
Re-implements the agent-runtime discipline (see nexos-agent-runtime skill):
  - HARD overall timeout (never hang the loop)
  - heartbeat progress polling (distinguish slow-boot vs dead)
  - robust QEMU tree-kill on exit (no lingering process locking os.img)
  - structured log + SUMMARIZED verdict (truncate, report PASS/FAIL up front)
Confirms the 32->64 switch and long-mode boot far enough to install the IDT
(which exercises build_idt()'s RIP-relative LEA rebasing) with no crash marker.
Works on clean release builds (no [K64-VERIFY] self-test required).
Windows QEMU (D:\\qemu), tcg accelerator.
"""
import os, sys, time, subprocess

IMG   = "build/os.img"
LOG   = "build/serial_boot_%d.log" % os.getpid()   # unique per run: no stale reads
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
HARD_TIMEOUT = 90.0          # seconds; whole test must finish within this
POLL_INTERVAL = 0.5
HEARTBEAT_EVERY = 5          # print a progress tick every N polls

def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""

def main():
    t0 = time.time()
    if not os.path.exists(QEMU):
        print("FAIL: QEMU not found: %s" % QEMU); sys.exit(2)
    if not os.path.exists(IMG):
        print("FAIL: IMG not found: %s" % IMG); sys.exit(2)
    # NOTE: safe-delete hooks in some sandboxes fail-closed on os.remove when
    # the recycle bin is unavailable.  A unique per-run log name means there is
    # nothing stale to remove; tolerate the failure just in case.
    if os.path.exists(LOG):
        try:
            os.remove(LOG)
        except OSError:
            pass

    errf = open("build/boot_err.log", "wb")
    q = subprocess.Popen([
        QEMU, "-m", "512M", "-accel", "tcg", "-display", "none", "-no-reboot",
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    checks = []
    def wait_for(needle, timeout):
        end = time.time() + timeout
        n = 0
        while time.time() < end:
            if needle in ser_text():
                return True
            n += 1
            if n % HEARTBEAT_EVERY == 0:
                print("  ... waiting for %s (%.0fs)" % (needle, time.time() - t0))
            if time.time() - t0 > HARD_TIMEOUT:
                return False
            time.sleep(POLL_INTERVAL)
        return False

    try:
        ok = True
        if not wait_for("[K64-1] kmain64 entered", timeout=60.0):
            print("FAIL: 64-bit kernel never entered"); ok = False
        else:
            print("OK: 64-bit kernel entered ([K64-1])")

        if not wait_for("[K64-IDT] installed", timeout=30.0):
            print("FAIL: IDT never installed (build_idt likely crashed)"); ok = False
        else:
            print("OK: IDT installed ([K64-IDT])")

        t = ser_text()
        crash = ("PANIC" in t or "triple fault" in t or "Exception #" in t
                 or "DABT" in t or "#PF" in t or "GPF" in t)
        if crash:
            print("FAIL: crash markers in serial"); ok = False
        else:
            print("OK: no crash markers")

        print("RESULT: " + ("PASS" if ok else "FAIL")
              + "  (%.1fs)" % (time.time() - t0))
        return 0 if ok else 1
    finally:
        # robust tree-kill so no QEMU lingers and locks os.img
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception: pass
        try:
            subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue"
                 " | Stop-Process -Force -ErrorAction SilentlyContinue"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        except Exception:
            pass

if __name__ == "__main__":
    sys.exit(main())
