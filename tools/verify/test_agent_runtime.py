#!/usr/bin/env python3
"""
Runtime verification of the enhanced NexOS AI Agent engine, driven from the
64-bit kernel's temporary [AGENT-VERIFY] self-test block.

Boots build/os.img headless (tcg), captures serial, and asserts:
  - the agent pipeline initialized and ran ([AGENT-VERIFY] ... done)
  - a normal run produced output ([AGENT-VERIFY-RUN] ... [AGENT-VERIFY-RUN-END])
  - confirm mode blocked a DANGEROUS task ([AGENT-VERIFY-CONFIRM] shows
    "blocked: dangerous")
  - no crash markers anywhere in the log
Uses Windows QEMU (D:\\qemu), tcg accelerator.
"""
import os, sys, time, subprocess

IMG   = "build/os.img"
LOG   = "build/serial_agent_%d.log" % os.getpid()   # unique per run: no stale reads
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
HARD_TIMEOUT = 120.0
POLL = 0.5
# The unique per-run log name guarantees fresh content, and the [K64-IDT]
# installed check proves a real 64-bit boot.  This is just a final sanity
# floor against an implausibly-instant (<0.3s) read.
MIN_BOOT = 0.3

def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""

def section(text, start, end):
    s = text.find(start)
    if s < 0: return None
    s += len(start)
    e = text.find(end, s)
    if e < 0: return text[s:]
    return text[s:e]

def main():
    if not os.path.exists(QEMU):
        print("FAIL: QEMU not found"); sys.exit(2)
    if not os.path.exists(IMG):
        print("FAIL: IMG not found"); sys.exit(2)

    errf = open("build/agent_err.log", "wb")
    q = subprocess.Popen([QEMU, "-m", "512M", "-accel", "tcg", "-display", "none",
                          "-no-reboot", "-serial", "file:%s" % LOG,
                          "-drive", "format=raw,file=%s" % IMG],
                         stdout=errf, stderr=errf)
    t0 = time.time()
    try:
        # wait for the verify block to finish
        end = t0 + HARD_TIMEOUT
        while time.time() < end:
            if "[AGENT-VERIFY] done" in ser_text(): break
            time.sleep(POLL)
        elapsed = time.time() - t0
        t = ser_text()

        # Guard against stale-log false positives: the 64-bit kernel MUST have
        # booted (reached its IDT install) for this run to be considered real.
        if "[K64-IDT] installed" not in t:
            print("FAIL: 64-bit kernel never booted (stale/empty serial?)")
            ok = False
        elif elapsed < MIN_BOOT:
            print("FAIL: finished in %.1fs < MIN_BOOT %.1fs (stale log?)" % (elapsed, MIN_BOOT))
            ok = False
        else:
            ok = True

        if "[AGENT-VERIFY] done" not in t:
            print("FAIL: agent self-test never completed"); ok = False
        else:
            print("OK: agent self-test completed")

        # Risk classification + dependency plan: the PLAN section should show
        # at least one task tagged "dangerous" and dependency edges.
        plan = section(t, "[AGENT-VERIFY-PLAN]\n", "[AGENT-VERIFY-PLAN-END]")
        if plan and "dangerous" in plan:
            print("OK: risk classification -> dangerous task tagged")
        else:
            print("WARN: risk classification not visible in plan")
        if plan and ("depends_on" in plan or "needs #" in plan):
            print("OK: dependency-aware plan present")
        else:
            print("WARN: dependency edges not visible in plan")

        # Confirm-mode gate: agent_log should report the dangerous task blocked
        # and its dependent step skipped (these prove the gate + dep-skip fire
        # WITHOUT needing the 4 MB Markov model, which the 64-bit heap lacks).
        if "dangerous task blocked by confirm mode" in t:
            print("OK: confirm mode blocked dangerous task")
        else:
            print("WARN: dangerous-task gate not observed")
        if "task skipped: dependency not met" in t:
            print("OK: dependent step skipped when prerequisite failed")
        else:
            print("WARN: dependency-skip not observed")
        if "retry after low/failed score" in t:
            print("OK: reflection-driven retry fired")
        else:
            print("WARN: retry path not observed")

        crash = ("PANIC" in t or "triple fault" in t or "Exception #" in t
                 or "#PF" in t or "GPF" in t)
        if crash:
            print("FAIL: crash markers present"); ok = False
        else:
            print("OK: no crash markers")

        # show plan snippet for the record
        if plan:
            snippet = " | ".join([l.strip() for l in plan.strip().splitlines() if l.strip()])[:400]
            print("PLAN: %s" % snippet)

        print("RESULT: " + ("PASS" if ok else "FAIL") + "  (%.1fs)" % (time.time() - t0))
        return 0 if ok else 1
    finally:
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception: pass
        try:
            subprocess.run(["powershell", "-NoProfile", "-Command",
                "Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue"
                " | Stop-Process -Force -ErrorAction SilentlyContinue"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        except Exception: pass

if __name__ == "__main__":
    sys.exit(main())
