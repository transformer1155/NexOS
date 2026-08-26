#!/usr/bin/env python3
"""Headless boot of os.img, capture serial to a file, then dump the tail.

Reproduces the 64-bit GUI crash that shows up as "闪退" under a real display:
the 32-bit kernel auto-switches to 64-bit and enters the Win11 desktop, which
triple-faults.  With -display none + -no-reboot the VM simply halts, so the
serial log shows exactly the last messages before the crash.
"""
import os, sys, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
LOG = "build/boot_crash.log"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 25

if os.path.exists(LOG):
    os.remove(LOG)
args = [QEMU, "-machine", "pc", "-drive", "format=raw,file=%s" % IMG,
        "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser"]
print("[run] " + " ".join(args))
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(SECS)
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()
with open(LOG, "rb") as fh:
    data = fh.read().decode("latin-1", "ignore")
print("\n=== serial log (%d bytes) ===" % len(data))
lines = data.splitlines()
for ln in lines[-60:]:
    print(ln)
print("\n=== last 8 lines (crash region) ===")
for ln in lines[-8:]:
    print(">>", ln)
