#!/usr/bin/env python3
"""Reproduce + verify the taskbar Terminal pin behaviour.
  Scenario A (fresh):  click the taskbar Terminal pin -> GUI must exit
                        (drop to text shell), regardless of any open window.
  Scenario B (window): open a managed TerminalApp window (right-click a
                        file -> Open with -> Terminal), then click the
                        taskbar Terminal pin -> GUI must STILL exit.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_tt.img"
MPORT = 4491
SPORT = 4492
SER_BUF = bytearray()
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    sock.settimeout(0.5)
    logf = open("build/serial_tt.log", "ab")
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        logf.write(data); logf.flush()
        with _SER_LOCK:
            SER_BUF.extend(data)
    logf.close()


def log_text():
    with _SER_LOCK:
        return SER_BUF.decode("utf-8", "replace")


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in log_text():
            return True
        time.sleep(0.25)
    return False


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', '>': 'shift-dot', ':': 'shift-semicolon'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


STEP = 100


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.18):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def rclick(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 3\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(f"screendump {path}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared")


def ppm_dims(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        return int(wh[0]), int(wh[1])


def term_pin_xy(W, H):
    # mirrors Desktop.cs GroupX / Taskbar layout
    BtnSz, BtnGap, tN = 40, 6, 7
    GroupW = (tN + 1) * BtnSz + tN * BtnGap
    bx = (W - GroupW) // 2
    if bx < 8:
        bx = 8
    i = 1  # Terminal is the 2nd pin
    x = bx + (i + 1) * (BtnSz + BtnGap) + BtnSz // 2
    y = (H - 48) + (48 - BtnSz) // 2 + BtnSz // 2
    return x, y


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_tt.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    checks = {}
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)")
            return 1
        time.sleep(2.0)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)

        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (no gui)")
            return 1
        time.sleep(1.5)

        # resolution
        grab(mon, "build/tt_a.ppm")
        W, H = ppm_dims("build/tt_a.ppm")
        print(f"resolution {W}x{H}")
        tx, ty = term_pin_xy(W, H)
        print(f"taskbar Terminal pin at ({tx},{ty})")

        # ---- Scenario A: fresh click --------------------------------
        click(mon, tx, ty)
        checks["A_fresh_exit"] = wait_for_log("[GUI] Exited GUI mode", 30.0)
        print("A fresh click exits GUI:", checks["A_fresh_exit"])
        if checks["A_fresh_exit"]:
            time.sleep(0.5)
            type_line(mon, "session clear"); time.sleep(0.5)

        # ---- re-enter and open a managed Terminal window ------------
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Re-entered" , 5.0):
            wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)

        # Double-click This PC (64,64) -> File Explorer
        click(mon, 64, 64, dbl=True)
        time.sleep(1.5)
        # nav to System volume
        click(mon, 456, 363)
        time.sleep(0.8)
        # right-click first file row
        rclick(mon, 781, 319)
        time.sleep(0.8)
        # "Open with..." item
        click(mon, 810, 444)
        time.sleep(0.8)
        # "Terminal" (2nd row of sub-menu)
        click(mon, 810, 384)
        time.sleep(1.5)
        # a terminal window opened -> GUI still active, log a marker
        grab(mon, "build/tt_win.ppm")
        # ---- Scenario B: click taskbar Terminal pin ------------------
        click(mon, tx, ty)
        checks["B_window_exit"] = wait_for_log("[GUI] Exited GUI mode", 30.0)
        print("B click-with-terminal-window exits GUI:", checks["B_window_exit"])

        ok = checks.get("A_fresh_exit", False) and checks.get("B_window_exit", False)
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon:
                mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except Exception:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
