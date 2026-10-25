#!/usr/bin/env python3
"""Test whether small relative mouse moves keep working across many commands
and whether mouse_button still fires, WITHOUT the -10000 slam."""
import os, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
PORT = 4464

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
    for f in ("build/mv2_work.img", "build/serial_mv2.log"):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run(["cp", "build/os.img", "build/mv2_work.img"], check=True)
    errf = open("build/qemu_mv2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/mv2_work.img",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", "file,id=ser,path=build/serial_mv2.log",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT)
        mon.settimeout(2.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        type_line(mon, "gui")
        time.sleep(12.0)

        shot(mon, "n1")
        w, h, p1 = read_ppm("build/n1.ppm")
        print("shot1 %dx%d" % (w, h))

        # small move #1
        mon.sendall(b"mouse_move 50 30\n"); time.sleep(0.3)
        shot(mon, "n2")
        _, _, p2 = read_ppm("build/n2.ppm")
        n, box = bbox_diff(w, h, p1, p2)
        print("move(50,30): changed=%d box=%s" % (n, box))

        # small move #2
        mon.sendall(b"mouse_move 50 30\n"); time.sleep(0.3)
        shot(mon, "n3")
        _, _, p3 = read_ppm("build/n3.ppm")
        n, box = bbox_diff(w, h, p2, p3)
        print("move(50,30) again: changed=%d box=%s" % (n, box))

        # back
        mon.sendall(b"mouse_move -100 -60\n"); time.sleep(0.3)
        shot(mon, "n4")
        _, _, p4 = read_ppm("build/n4.ppm")
        n, box = bbox_diff(w, h, p3, p4)
        print("move(-100,-60): changed=%d box=%s" % (n, box))

        # bigger move 400,300
        mon.sendall(b"mouse_move 400 300\n"); time.sleep(0.3)
        shot(mon, "n5")
        _, _, p5 = read_ppm("build/n5.ppm")
        n, box = bbox_diff(w, h, p4, p5)
        print("move(400,300): changed=%d box=%s" % (n, box))

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

if __name__ == "__main__":
    main()
