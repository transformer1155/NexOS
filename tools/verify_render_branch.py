#!/usr/bin/env python3
"""Verify which render_all() branch the 64-bit kernel actually takes.

Boots the BIOS raw image (build/os.img) headlessly, captures serial, and
takes a screendump so we can tell apart:

  [GUI] render_all edge-test  -> g_edge_test temp branch (light-gray 0xD8DCE2,
                                 no wallpaper / no topbar)  == BAD
  [GUI] render_all begin      -> real desktop path (wallpaper + topbar)  == GOOD

BIOS/VBE keeps the framebuffer low, so this needs only a small guest, which
avoids the >4GB high-FB test's 5G guest requirement.

Usage: verify_render_branch.py [wait_seconds]
"""
import os
import re
import socket
import subprocess
import sys
import time

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_render_branch.txt")
SHOT_PPM = os.path.join(BUILD, "render_branch.ppm")
QERR = os.path.join(BUILD, "qemu_render_branch.err")
MON_PORT = 5591

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 45

for f in (SERIAL, SHOT_PPM, QERR):
    if os.path.exists(f):
        os.remove(f)

cmd = [
    QEMU,
    "-machine", "pc",
    "-m", "2G",
    "-accel", "tcg",
    "-drive", f"if=ide,format=raw,file={DISK}",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std",
    "-display", "none",
]

print("[*] launching QEMU (BIOS raw, 2G, tcg)...")
errf = open(QERR, "wb")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf)

# monitor may take a moment to bind; retry
s = None
for attempt in range(20):
    if p.poll() is not None:
        errf.close()
        with open(QERR, "r", errors="replace") as fh:
            err = fh.read().strip()
        print(f"[FATAL] QEMU exited early (rc={p.returncode})")
        print("        stderr:", err[:400] or "(empty)")
        sys.exit(2)
    try:
        s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=3)
        break
    except OSError:
        time.sleep(0.5)
if s is None:
    p.kill()
    errf.close()
    print("[FATAL] could not connect to QEMU monitor")
    sys.exit(2)
s.settimeout(2.0)


def drain():
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except OSError:
        pass
    return buf.decode(errors="replace")


drain()
print(f"[*] booting, waiting {WAIT}s ...")
time.sleep(WAIT)

# screendump for pixel evidence
s.sendall(f"screendump {SHOT_PPM}\n".encode())
time.sleep(2.5)
drain()
s.sendall(b"quit\n")
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
errf.close()

# ---- analyse serial ----
serial = ""
if os.path.exists(SERIAL):
    with open(SERIAL, "r", errors="replace") as fh:
        serial = fh.read()

edge = serial.count("render_all edge-test")
real = serial.count("render_all begin")
entered = "Entered GUI mode" in serial

print("\n================ SERIAL EVIDENCE ================")
print(f"  'Entered GUI mode'          : {entered}")
print(f"  'render_all edge-test' hits : {edge}   <- BAD (temp gray branch)")
print(f"  'render_all begin'    hits  : {real}   <- GOOD (real desktop)")
for key in ("[GUI] Entered GUI mode", "render_all edge-test", "render_all begin",
            "64-bit high framebuffer", "fb_base="):
    for ln in serial.splitlines():
        if key in ln:
            print(f"    | {ln.strip()[:110]}")
            break

# ---- analyse screendump ----
print("\n================ PIXEL EVIDENCE ================")
if not os.path.exists(SHOT_PPM):
    print("  (no screendump produced)")
else:
    with open(SHOT_PPM, "rb") as fh:
        data = fh.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        print("  (unrecognised PPM header)")
    else:
        w, h = int(m.group(1)), int(m.group(2))
        px = data[m.end():]
        print(f"  frame: {w}x{h}")

        def sample(x, y):
            o = (y * w + x) * 3
            if o + 3 > len(px):
                return None
            return (px[o], px[o + 1], px[o + 2])

        rows = {"row 2 (topbar)": 2, "row 120 (wallpaper)": 120,
                "row 300 (mid)": 300, f"row {max(0,h-8)} (taskbar)": max(0, h - 8)}
        for label, y in rows.items():
            got = [sample(x, y) for x in (10, w // 2, max(0, w - 10))]
            print(f"  {label:<26}: {got}")

        # count distinct colours to see if the screen is a flat fill
        seen = {}
        for y in range(0, h, max(1, h // 60)):
            for x in range(0, w, max(1, w // 60)):
                c = sample(x, y)
                if c:
                    seen[c] = seen.get(c, 0) + 1
        top = sorted(seen.items(), key=lambda kv: -kv[1])[:5]
        print("  dominant colours (sampled):")
        for c, n in top:
            print(f"    rgb{c}  x{n}")
        gray = (0xD8, 0xDC, 0xE2)
        flat_gray = top and top[0][0] == gray
        print(f"\n  edge-test gray rgb{gray} dominant? {bool(flat_gray)}")

print("\n================ VERDICT ================")
if edge > 0:
    print("  BAD : g_edge_test temp branch is ACTIVE -> desktop is the")
    print("        light-gray edge-test frame, NOT the real desktop.")
elif real > 0 and entered:
    print("  GOOD: real desktop render path is running (wallpaper + topbar).")
elif entered:
    print("  PARTIAL: entered GUI mode but no render_all marker seen.")
else:
    print("  FAIL: never entered GUI mode.")
print(f"\nserial : {SERIAL}")
print(f"shot   : {SHOT_PPM}")
