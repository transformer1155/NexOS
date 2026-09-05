#!/usr/bin/env python3
"""Diagnose two reported issues in one session:

  A) After `gui`, the desktop is mostly black until some action (right-click,
     opening an app) triggers a full repaint -> "mouse reveals pixels as it
     moves".  We screendump IMMEDIATELY after Entered GUI mode (no mouse move)
     and again after a right-click (full repaint) to compare completeness.

  B) Clicking the desktop Terminal icon -> gui_exit() -> black screen instead
     of the text shell.  We click the icon, wait for the exit log, screendump
     the (text-mode) screen, and check the shell still answers commands.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_term_diag.img"
MPORT = 4466
SPORT = 4467
D1 = "build/diag_after_gui.ppm"      # immediately after entering GUI
D2 = "build/diag_after_rclick.ppm"   # after a right-click (full repaint)
D3 = "build/diag_after_term.ppm"     # after clicking Terminal icon (text mode?)

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


def _ser_reader(sock, logf):
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
        with _SER_LOCK:
            SER_BUF.extend(data)
        try:
            logf.write(data); logf.flush()
        except OSError:
            pass


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
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


STEP = 100


def _rel(mon, dx, dy):
    mon.sendall(("mouse_move %d %d\n" % (dx, dy)).encode())
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
    time.sleep(0.25)


def left_click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.45)


def right_click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 2\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.45)


def grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(f"screendump {path}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared")


def black_ratio(path):
    # Wait for a complete PPM (QEMU writes screendumps asynchronously).
    for _ in range(40):
        try:
            f = open(path, "rb")
            hdr = f.readline()
            if hdr.strip() != b"P6":
                f.close(); time.sleep(0.15); continue
            f.readline(); f.readline()
            d = f.read()
            f.close()
            if len(d) < 1000:
                time.sleep(0.15); continue
            n = len(d) // 3
            black = sum(1 for i in range(0, len(d), 3) if d[i] == 0 and d[i + 1] == 0 and d[i + 2] == 0)
            return black / n if n else 1.0
        except OSError:
            time.sleep(0.15)
    raise RuntimeError(f"could not read complete PPM {path}")


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (D1, D2, D3):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_term_diag.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = ser = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        logf = open("build/serial_diag.log", "wb")
        threading.Thread(target=_ser_reader, args=(ser, logf), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)")
            return 1
        time.sleep(2.0)
        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)

        # ---- A: enter GUI, screendump immediately ------------------------
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (never entered GUI)")
            return 1
        # Wait a moment for the shell to finish painting, then grab WITHOUT
        # moving the mouse (so no present_rect has refreshed anything).
        time.sleep(0.8)
        grab(mon, D1)
        br1 = black_ratio(D1)
        print(f"A1 immediate-after-gui black ratio: {br1:.3f}")

        # ---- B: click the Terminal desktop icon (no right-click first, so
        # no context menu can swallow the click) -> gui_exit() -------------
        # Desktop.cs Put(1, Kind.Terminal) -> col=0 row=1 -> center (64,156)
        left_click(mon, 64, 156)
        exited = wait_for_log("[GUI] Exited GUI mode", 30.0)
        time.sleep(1.0)
        type_line(mon, "echo term-ok")
        alive = wait_for_log("term-ok", 20.0)
        print(f"B Terminal-click exited={exited} shell_alive={alive}")
        print(f"B: {'OK (shell back after Terminal click)' if exited and alive else 'BUG (no shell after Terminal click)'}")

        # ---- C: re-enter GUI, then ESC should also drop back -------------
        type_line(mon, "gui")
        gui2 = wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.0)
        mon.sendall(b"sendkey esc\n"); time.sleep(0.6)
        esc_exit = wait_for_log("[GUI] Exited GUI mode", 30.0)
        time.sleep(1.0)
        # The screen after ESC must show the text terminal (not stay black).
        D_ESC = "build/diag_after_esc.ppm"
        if os.path.exists(D_ESC):
            os.remove(D_ESC)
        try:
            grab(mon, D_ESC)
            br_esc = black_ratio(D_ESC)
            print(f"C screen after ESC: black ratio {br_esc:.3f}")
        except Exception as e:
            print(f"C screendump after ESC failed: {e}")
            br_esc = 1.0
        type_line(mon, "echo esc-ok")
        alive2 = wait_for_log("esc-ok", 20.0)
        print(f"C re-enter={gui2} ESC-exit={esc_exit} screen_ok={br_esc < 0.6} shell_alive={alive2}")

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        n_exit = slog.count("[GUI] Exited GUI mode")
        print(f"total exits={n_exit} no_fault={not fault}")
        ok = (br1 < 0.3) and exited and alive and esc_exit and alive2 and not fault \
             and br_esc < 0.6
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
