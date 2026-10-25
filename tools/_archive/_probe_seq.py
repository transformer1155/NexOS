#!/usr/bin/env python3
"""Reproduce the test_rclick tray failure step by step."""
import os, socket, time, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
PORT = 4466
_CURSOR = [640, 360]

def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)

def mouse_abs(mon, x, y):
    global _CURSOR
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.08)
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
    time.sleep(0.2)

def press(mon, btn):
    mon.sendall(("mouse_button %d\n" % btn).encode())
    time.sleep(0.15)

def rclick(mon, x, y):
    mouse_abs(mon, x, y)
    press(mon, 2); time.sleep(0.6); press(mon, 0); time.sleep(0.3)

def lclick(mon, x, y):
    mouse_abs(mon, x, y)
    press(mon, 1); time.sleep(0.1); press(mon, 0); time.sleep(0.25)

def read_ppm(path):
    for _ in range(10):
        try:
            with open(path, "rb") as f:
                f.readline()
                w, h = (int(x) for x in f.readline().split())
                f.readline()
                px = f.read()
            if len(px) >= w * h * 3:
                return w, h, px
        except (AssertionError, IndexError, OSError):
            pass
        time.sleep(0.5)
    raise RuntimeError("ppm read failed")

def shot(mon, name):
    mon.sendall(("screendump build/%s.ppm\n" % name).encode())
    time.sleep(1.0)

def bbox_diff(w, h, pa, pb, tol=24):
    x0, y0, x1, y1 = w, h, -1, -1
    n = 0
    for yy in range(h):
        row = yy * w * 3
        for xx in range(w):
            i = row + xx * 3
            if (abs(pa[i]-pb[i]) + abs(pa[i+1]-pb[i+1]) + abs(pa[i+2]-pb[i+2])) > tol:
                n += 1
                if xx < x0: x0 = xx
                if xx > x1: x1 = xx
                if yy < y0: y0 = yy
                if yy > y1: y1 = yy
    return (n, (x0, y0, x1, y1))

def main():
    for f in ("build/seq_work.img", "build/serial_seq.log"):
        if os.path.exists(f): os.remove(f)
    subprocess.run(["cp", "build/os.img", "build/seq_work.img"], check=True)
    errf = open("build/qemu_seq.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/seq_work.img",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", "file,id=ser,path=build/serial_seq.log",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT)
        mon.settimeout(2.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        time.sleep(8.0)
        type_line(mon, "root"); type_line(mon, "admin"); type_line(mon, "gui")
        time.sleep(12.0)

        shot(mon, "q0")
        w, h, prev = read_ppm("build/q0.ppm")
        print("start %dx%d cursor=%s" % (w, h, _CURSOR))

        rclick(mon, 640, 400)
        shot(mon, "q1")
        _, _, p = read_ppm("build/q1.ppm")
        n, b = bbox_diff(w, h, prev, p); print("wall rclick: chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        lclick(mon, 5, 5)
        shot(mon, "q2")
        _, _, p = read_ppm("build/q2.ppm")
        n, b = bbox_diff(w, h, prev, p); print("dismiss1:   chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        rclick(mon, 505, 690)
        shot(mon, "q3")
        _, _, p = read_ppm("build/q3.ppm")
        n, b = bbox_diff(w, h, prev, p); print("pin rclick:  chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        lclick(mon, 5, 5)
        shot(mon, "q4")
        _, _, p = read_ppm("build/q4.ppm")
        n, b = bbox_diff(w, h, prev, p); print("dismiss2:   chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        rclick(mon, 900, 690)
        shot(mon, "q5")
        _, _, p = read_ppm("build/q5.ppm")
        n, b = bbox_diff(w, h, prev, p); print("bar rclick:  chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        lclick(mon, 5, 5)
        shot(mon, "q6")
        _, _, p = read_ppm("build/q6.ppm")
        n, b = bbox_diff(w, h, prev, p); print("dismiss3:   chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        rclick(mon, 1115, 695)
        shot(mon, "q7")
        _, _, p = read_ppm("build/q7.ppm")
        n, b = bbox_diff(w, h, prev, p); print("tray rclick: chg=%d box=%s cursor=%s" % (n, b, _CURSOR))
        prev = p

        rclick(mon, 1115, 695)
        shot(mon, "q8")
        _, _, p = read_ppm("build/q8.ppm")
        n, b = bbox_diff(w, h, prev, p); print("tray rclick2: chg=%d box=%s cursor=%s" % (n, b, _CURSOR))

        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()

if __name__ == "__main__":
    main()
