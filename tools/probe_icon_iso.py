#!/usr/bin/env python3
"""Isolated 64-bit desktop-icon right-click probe (no prior menus open)."""
import os, sys, socket, time, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/iso_work.img"
LOG = "build/serial_iso.log"
PORT = 4469


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    km = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
          '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        k = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else km.get(ch, ch)
        mon.sendall(f"sendkey {k}\n".encode()); time.sleep(0.08)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.4)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline(); px = f.read()
    return w, h, px


def read_ppm_retry(path, tries=10):
    for _ in range(tries):
        try:
            w, h, px = read_ppm(path)
            if len(px) >= w * h * 3:
                return w, h, px
        except Exception:
            pass
        time.sleep(0.5)
    return read_ppm(path)


def region_diff(a, b, x0, y0, rw, rh, tol=24):
    w, h, pa = read_ppm_retry(a); _, _, pb = read_ppm_retry(b)
    x0 = max(0, min(x0, w - 1)); y0 = max(0, min(y0, h - 1))
    x1 = min(w, x0 + rw); y1 = min(h, y0 + rh)
    n = 0
    for yy in range(y0, y1):
        row = yy * w * 3
        for xx in range(x0, x1):
            i = row + xx * 3
            if abs(pa[i]-pb[i])+abs(pa[i+1]-pb[i+1])+abs(pa[i+2]-pb[i+2]) > tol:
                n += 1
    return n


def mouse_abs(mon, x, y):
    cx, cy = 640, 360
    while abs(x-cx) > 80 or abs(y-cy) > 80:
        sx = max(-80, min(80, x-cx)); sy = max(-80, min(80, y-cy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.1); cx += sx; cy += sy
    dx, dy = x-cx, y-cy
    if dx or dy:
        mon.sendall(("mouse_move %d %d\n" % (dx, dy)).encode()); time.sleep(0.15)


def mpress(mon, b):
    mon.sendall(("mouse_button %d\n" % b).encode()); time.sleep(0.15)


subprocess.run(["cp", IMG, WORK], check=True)
for f in (LOG, "build/iso_base.ppm", "build/iso_menu.ppm"):
    if os.path.exists(f):
        os.remove(f)
qemu = subprocess.Popen([
    "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}", "-m", "128M",
    "-vga", "std", "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    "-chardev", f"file,id=ser,path={LOG}", "-serial", "chardev:ser",
], stderr=open("build/qemu_iso.err", "wb"))
try:
    mon = wait_sock(PORT); mon.settimeout(3.0)
    try:
        mon.recv(65536)
    except Exception:
        pass
    time.sleep(8.0)
    type_line(mon, "root"); type_line(mon, "admin"); time.sleep(1.0)
    type_line(mon, "switch64"); time.sleep(10.0)
    type_line(mon, "gui"); time.sleep(12.0)
    # fresh state: right-click the icon at (68,68) in isolation
    for idx in range(4):
        col = idx // 5; row = idx % 5
        x = 22 + col * 92 + 46; y = 22 + row * 92 + 46
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.05)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.4)  # dismiss any menu
        mon.sendall(b"screendump build/iso_base.ppm\n"); time.sleep(1.0)
        mouse_abs(mon, x, y); mpress(mon, 2); time.sleep(0.6); mpress(mon, 0)
        time.sleep(0.4)
        mon.sendall(b"screendump build/iso_menu.ppm\n"); time.sleep(1.0)
        d = region_diff("build/iso_base.ppm", "build/iso_menu.ppm",
                        max(0, x-170), max(0, y-10), 340, 430)
        print(f"icon idx={idx} ({x},{y}): changed px={d}")
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try:
        qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate(); qemu.wait(timeout=3.0)
