#!/usr/bin/env python3
"""Boot os.img with a real (headless) framebuffer, log in, run `gui`, then
screendump the desktop and measure how much of it is non-black + whether the
managed taskbar band (bottom dark strip) is present.  Proves the Win11 desktop
actually renders pixels instead of black-screen / crashing.

Usage: python3 tools/verify_gui_desktop.py [IMG] [SECS]
"""
import os, sys, subprocess, time, socket, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 35
PPM = "build/gui_desktop.ppm"
LOG = "build/gui_desktop.log"
ERR = "build/gui_desktop.err"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

for f in (PPM, LOG, ERR):
    if os.path.exists(f):
        os.remove(f)

MONPORT = 55999
args = [QEMU, "-machine", "pc", "-drive", "format=raw,file=%s" % IMG,
        "-m", "256M", "-accel", "tcg,tb-size=128", "-vga", "std",
        "-display", "egl-headless", "-no-reboot",
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % MONPORT]
print("[run] " + " ".join(args))
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(ERR, "wb"))
time.sleep(3)

def mon(cmd):
    last = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", MONPORT), timeout=2)
            s.recv(4096)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.3)
            try:
                s.recv(4096)
            except Exception:
                pass
            s.close()
            return True
        except Exception as ex:
            last = ex
            time.sleep(0.3)
    print("[mon] failed: %s (%s)" % (cmd, last))
    return False

def type_line(s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon("sendkey %s" % key)
        time.sleep(0.07)
    mon("sendkey ret")
    time.sleep(0.4)

def wait_for(substr, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(LOG, "rb") as f:
                txt = f.read().decode("latin-1", "ignore")
            if substr in txt:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False

# Give the boot a fixed head-start (login prompt appears ~10s in).
time.sleep(8)
print("[harness] typing root")
type_line("root")
time.sleep(1)
print("[harness] typing admin")
type_line("admin")
time.sleep(2)
if wait_for("Shell ready", timeout=15):
    print("[harness] shell ready -> typing gui")
    type_line("gui")
else:
    print("[harness] WARN: shell not ready; serial tail:")
    try:
        print(open(LOG).read()[-600:])
    except Exception:
        pass
# give the desktop time to paint
time.sleep(10)
mon("screendump %s" % PPM)
time.sleep(1)
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()

# ---- analyze PPM (P6, what QEMU's screendump actually emits) ----
def analyze(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"P6":
        return None
    i = 2
    nums = []
    while len(nums) < 3:
        while i < len(d) and d[i] in b" \t\r\n":
            i += 1
        s = i
        while i < len(d) and d[i] in b"0123456789":
            i += 1
        nums.append(int(d[s:i]))
        i += 1
    w, h, maxval = nums
    off = i
    if maxval > 255:
        off += 1
    px = d[off:]
    nonblack = 0
    total = 0
    band_nonblack = 0
    band_total = 0
    rowbytes = w * 3
    for y in range(h):
        row = px[y * rowbytes:(y + 1) * rowbytes]
        for x in range(w):
            j = x * 3
            if j + 2 >= len(row):
                break
            r, g, b = row[j], row[j + 1], row[j + 2]
            dark = (r < 12 and g < 12 and b < 12)
            if not dark:
                nonblack += 1
            total += 1
            if y >= h - 48:
                if not dark:
                    band_nonblack += 1
                band_total += 1
    if total == 0:
        return None
    return w, h, 100.0 * nonblack / total, 100.0 * band_nonblack / band_total

res = analyze(PPM) if os.path.exists(PPM) else None
print("\n=== serial tail ===")
try:
    print(open(LOG).read()[-1500:])
except Exception:
    print("(no serial log)")
print("\n=== QEMU stderr tail ===")
try:
    print(open(ERR).read()[-800:])
except Exception:
    print("(no stderr)")
print("\n=== GUI desktop verification ===")
if res is None:
    print("RESULT: FAIL (no screendump produced)")
else:
    w, h, pct, band = res
    print("PPM: %dx%d  non-black=%.1f%%  bottom48px band non-black=%.1f%%" % (w, h, pct, band))
    ok = pct > 5 and band > 30
    print("RESULT: %s (non-black>5%% AND taskbar-band>30%%)" % ("PASS" if ok else "FAIL"))
