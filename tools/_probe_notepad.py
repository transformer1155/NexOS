#!/usr/bin/env python3
"""Probe: open the SFS File Explorer and screenshot the layout so we can
read the exact file-list coordinates (used to author test_win32_notepad.py)."""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_probe.img"
MPORT = 4501
SPORT = 4502
SER_BUF = bytearray()
_LOCK = threading.Lock()


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _reader(sock):
    sock.settimeout(0.5)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        with _LOCK:
            SER_BUF.extend(data)


def log_text():
    with _LOCK:
        return SER_BUF.decode("utf-8", "replace")


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in log_text():
            return True
        time.sleep(0.25)
    return False


def type_line(mon, s):
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else ch
        mon.sendall(f"sendkey {key}\n".encode()); time.sleep(0.08)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.4)


STEP = 100


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode()); time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx); dy = min(STEP, y - cy)
        _rel(mon, dx, dy); cx += dx; cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(f"screendump {path}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25); return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared")


def png(src, dst):
    try:
        from PIL import Image
        Image.open(src).save(dst); return dst
    except Exception:
        return src


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_probe.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        wait_for_log("[K5] Hello world written", 90.0)
        time.sleep(2.0)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0): break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        type_line(mon, "gui")
        wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.8)
        # Portal desktop: first tile is "This PC" and opens File Explorer.
        click(mon, 318, 130, dbl=True, gap=0.18)   # This PC tile
        time.sleep(1.5)
        click(mon, 456, 363)                       # System (SFS) volume
        time.sleep(1.2)
        grab(mon, "build/probe.ppm")
        print("PROBE shot:", png("build/probe.ppm", "build/probe.png"))
        print("DONE")
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except OSError: pass
        time.sleep(0.5)
        qemu.kill(); errf.close()


if __name__ == "__main__":
    sys.exit(main())
