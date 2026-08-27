#!/usr/bin/env python3
"""Headless verification for Stage 8.1 SMP bring-up.

Boots build/os_textboot.img with QEMU -smp 4, logs in, types `switch` to
enter the 64-bit kernel, and captures the serial log.  Asserts that the
SMP foundation works:
  [SMP] init            - smp_init() ran on BSP
  [SMP] BSP apic_id=    - local APIC detected
  CPU0 online ...       - each AP reported in (may be CPU1/CPU2/CPU3)
  [SMP] online cpus=    - final count (expect >= 2 with -smp 4)

Usage: python3 tools/verify_k64_smp.py
"""
import os, socket, subprocess, sys, time

# Avoid Windows console GBK encoding crashes on non-ASCII serial bytes.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
# Use the freshest WSL-built image (new filename each build defeats DrvFS cache).
import glob as _glob
_cands = sorted(_glob.glob("build/os_textboot_qemu_*.img"))
IMG = _cands[-1] if _cands else "build/os_textboot_qemu.img"
LOG = "build/k64_smp_verify.log"
ERR = "build/k64_smp_verify.err"
MON_PORT = 4478
QEMU = "D:/qemu/qemu-system-x86_64.exe"
SMP = "4"

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
    for f in (LOG, ERR):
        try:
            with open(f, "w"):
                pass
        except (FileNotFoundError, OSError):
            pass
    errf = open(ERR, "wb")
    q = subprocess.Popen([
        QEMU, "-m", "1024", "-smp", SMP, "-accel", "tcg,tb-size=128",
        "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(MON_PORT)
        if not wait_for("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot did not complete)")
            sys.exit(1)
        time.sleep(1.0)
        print("[*] 32-bit boot ok; sending login (root/admin)")
        if wait_for("login:", 8.0):
            type_line(mon, "root"); time.sleep(0.6); type_line(mon, "admin")
        else:
            type_line(mon, "root"); time.sleep(0.6); type_line(mon, "admin")
        time.sleep(1.0)
        if not wait_for("Shell ready", 20.0):
            print("[!] 'Shell ready' not seen; continuing anyway")
        print("[*] sending 'switch' to enter 64-bit kernel")
        type_line(mon, "switch")
        ok64 = wait_for("[K64-1] kmain64 entered", 40.0)
        print("64-bit kmain64 reached:", ok64)
        # Wait for SMP markers
        smp_init = wait_for("[SMP] init", 30.0)
        bsp = wait_for("[SMP] BSP apic_id=", 30.0)
        online = wait_for("[SMP] online cpus=", 90.0)
        time.sleep(3.0)
        txt = ser_text()
        print("---- FULL serial tail ----")
        for ln in txt.splitlines()[-50:]:
            print(repr(ln))
        print("----------------------------")
        cpu_onlines = sum(1 for ln in txt.splitlines()
                          if ln.strip().startswith("CPU") and "online" in ln)
        print("[SMP] init seen:", smp_init)
        print("[SMP] BSP seen:", bsp)
        print("[SMP] online-count seen:", online)
        print("CPU* online lines:", cpu_onlines)
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: q.wait(timeout=6)
        except Exception:
            q.terminate(); q.kill()
        errf.close()
    pass_smp = smp_init and bsp and online and cpu_onlines >= 1
    print("RESULT:", "PASS" if (ok64 and pass_smp) else "FAIL")
