#!/usr/bin/env python3
"""Probe: boot to GUI, verify mouse positioning via `info mouse`, then
right-click the wallpaper and check whether a context menu actually renders."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
WORK = "build/probe_work.img"
LOG = "build/serial_probe.log"
PORT = 4462
PPM = "build/probe_menu.ppm"

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

def cmd(mon, s):
    mon.sendall((s + "\n").encode())
    time.sleep(0.3)
    out = b""
    try:
        while True:
            chunk = mon.recv(65536)
            if not chunk:
                break
            out += chunk
    except (socket.timeout, TimeoutError):
        pass
    return out.decode("latin-1", "ignore")

def mouse_abs(mon, x, y):
    mon.sendall(b"mouse_move -10000 -10000\n")
    time.sleep(0.2)
    mon.sendall(("mouse_move %d %d\n" % (x, y)).encode())
    time.sleep(0.25)

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
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, PPM):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_probe.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
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

        print("mouse before:", cmd(mon, "info mouse").strip())
        mouse_abs(mon, 640, 400)
        print("mouse after abs(640,400):", cmd(mon, "info mouse").strip())
        mouse_abs(mon, 200, 200)
        print("mouse after abs(200,200):", cmd(mon, "info mouse").strip())

        mouse_abs(mon, 640, 400)
        mon.sendall(b"mouse_button 2\n")
        time.sleep(1.0)
        mon.sendall(b"mouse_button 0\n")
        time.sleep(1.0)
        mon.sendall(b"screendump build/probe_menu.ppm\n")
        time.sleep(1.5)
        w, h, px = read_ppm(PPM)
        print("screenshot %dx%d" % (w, h))

        # Find light (menu) rows above the taskbar.
        rows = {}
        for yy in range(0, h, 4):
            row = yy * w * 3
            cnt = 0
            for xx in range(0, w, 4):
                i = row + xx * 3
                if px[i] + px[i + 1] + px[i + 2] > 620:
                    cnt += 1
            if cnt > 3:
                rows[yy] = cnt
        if rows:
            ys = sorted(rows)
            # A compact menu = consecutive rows with similar counts.
            print("light rows: y=%d..%d (%d rows)" % (ys[0], ys[-1], len(ys)))
            print("sample:", [(y, rows[y]) for y in ys[:8]])
            print("sample:", [(y, rows[y]) for y in ys[-8:]])
        else:
            print("NO light rows (menu absent)")

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
