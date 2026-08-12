#!/usr/bin/env python3
"""Probe the tray right-click: dump the taskbar region after the click."""
import os, socket, time, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
PORT = 4465
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
        time.sleep(0.05)
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
    time.sleep(0.2)

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

def main():
    subprocess.run(["cp", "build/os.img", "build/tray_work.img"], check=True)
    for f in ("build/serial_tray.log",):
        if os.path.exists(f): os.remove(f)
    errf = open("build/qemu_tray.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/tray_work.img",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", "file,id=ser,path=build/serial_tray.log",
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

        # right-click a tray button (x~1115, y~695)
        mouse_abs(mon, 1115, 695)
        mon.sendall(b"mouse_button 2\n"); time.sleep(1.0)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.8)
        mon.sendall(b"screendump build/tray_probe.ppm\n"); time.sleep(1.2)
        w, h, px = read_ppm("build/tray_probe.ppm")
        print("shot %dx%d" % (w, h))

        # ASCII of the bottom-right region x=900..1280, y=560..720
        W0, H0 = 900, 560
        sub_w = 380
        sub_h = 160
        for yy in range(32):
            line = ""
            for xx in range(76):
                x0 = W0 + xx * sub_w // 76
                x1 = W0 + (xx + 1) * sub_w // 76
                y0 = H0 + yy * sub_h // 32
                y1 = H0 + (yy + 1) * sub_h // 32
                s = cnt = 0
                for y in range(y0, y1, 2):
                    row = y * w * 3
                    for x in range(x0, x1, 2):
                        i = row + x * 3
                        s += px[i] + px[i+1] + px[i+2]
                        cnt += 1
                v = s / cnt if cnt else 0
                if v > 620: line += "#"
                elif v > 480: line += "o"
                elif v > 360: line += "."
                else: line += " "
            print("%3d %s" % (H0 + yy * sub_h // 32, line))

        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()

if __name__ == "__main__":
    main()
