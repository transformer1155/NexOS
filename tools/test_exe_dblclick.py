#!/usr/bin/env python3
"""Headless test: double-clicking a .exe in the File Explorer EXECUTES it
through the PE loader instead of opening it in Notepad.

Semantics asserted:
  1. Double-click an .exe row -> the kernel's PE loader maps the image and
     executes it (or reports why it cannot).  Notepad is never involved.
  2. Double-click a .txt row  -> still opens in Notepad.

Coordinates come from tools/test_desktop_ui.py (1280x720 desktop):
  This PC icon (64,64); File Explorer window (380,222)-(900,582);
  "System (SFS)" volume button (456,363); first file row centre (781,319)
  and W.RowH = 34, so row i is at y = 319 + 34*i.

SFS files are packed sorted by sfs_gen.py; in this window only the first two
rows (appdata.txt, chrome.exe) are visible.  chrome.exe is a PE32+ (x64) image
and the 32-bit kernel rejects it with rc=-3, which is still proof that the
.exe took the PE loader path (not Notepad).
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_exedbl.img"
MPORT = 4491
SPORT = 4492

ROW0_Y = 319
ROW_H = 34
ROW_X = 781

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
    logf = open("build/serial_exedbl.log", "ab")
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
              '-': 'minus', '_': 'shift-minus', '>': 'shift-dot',
              ':': 'shift-semicolon'}
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
            time.sleep(0.25)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared")


def load_ppm(path):
    for _ in range(40):
        try:
            f = open(path, "rb")
            if f.readline().strip() != b"P6":
                f.close(); time.sleep(0.15); continue
            f.readline(); f.readline()
            d = f.read()
            f.close()
            if len(d) < 720 * 1280 * 3:
                time.sleep(0.15); continue
            return d
        except OSError:
            time.sleep(0.15)
    raise RuntimeError("could not read PPM")


def diff_px(d1, d2, x0, y0, x1, y1, tol=12):
    n = 0
    for y in range(y0, y1):
        r = y * 1280 * 3
        for x in range(x0, x1):
            i = r + x * 3
            a = (abs(d1[i] - d2[i]) + abs(d1[i + 1] - d2[i + 1])
                 + abs(d1[i + 2] - d2[i + 2]))
            if a > tol:
                n += 1
    return n


def png(src, dst):
    try:
        from PIL import Image
        Image.open(src).save(dst)
        return dst
    except Exception:
        return src


def open_explorer_on_sfs(mon):
    click(mon, 64, 64, dbl=True, gap=0.18)
    time.sleep(1.5)
    click(mon, 456, 363)
    time.sleep(0.9)


def main():
    shot = "--shot" in sys.argv
    subprocess.run(["cp", IMG, WORK], check=True)
    for stale in ("build/serial_exedbl.log",):
        if os.path.exists(stale):
            os.remove(stale)
    errf = open("build/qemu_exedbl.err", "wb")
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
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

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

        # ---- enter the managed (C#) desktop -------------------------------
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui)")
            return 1
        time.sleep(1.8)

        # ================= Phase 1: .exe -> PE loader =====================
        open_explorer_on_sfs(mon)
        before = load_ppm(grab(mon, "build/exedbl_before.ppm"))

        exe_y = ROW0_Y + 1 * ROW_H      # row 1 = chrome.exe (visible)
        click(mon, ROW_X, exe_y, dbl=True, gap=0.18)

        checks["files_saw_exe"] = wait_for_log(
            "[FILES] running PE image chrome.exe", 25.0)
        checks["gui_ran_loader"] = wait_for_log(
            "[GUI] executing PE image via the win32/win64 loader: chrome.exe", 25.0)
        checks["not_notepad_for_exe"] = (
            "[FILES] opening in Notepad: chrome.exe" not in log_text())
        checks["loader_reported"] = wait_for_log(
            "win32_run(chrome.exe) failed rc=-3", 25.0)

        time.sleep(1.2)
        after = load_ppm(grab(mon, "build/exedbl_after.ppm"))
        drew = diff_px(before, after, 380, 222, 900, 582)
        print(f"P1 pixels changed after chrome.exe double-click: {drew}")
        checks["error_popup_drawn"] = drew > 4000  # small but clear popup
        if shot:
            print("  shot:", png("build/exedbl_after.ppm", "build/exedbl_after.png"))

        # Dismiss the error popup with an outside click.
        click(mon, 250, 550)
        time.sleep(0.6)

        # ================= Phase 2: .txt still opens in Notepad ===========
        before_txt = load_ppm(grab(mon, "build/exedbl_before_txt.ppm"))
        click(mon, ROW_X, ROW0_Y, dbl=True, gap=0.18)   # row 0 = appdata.txt
        checks["txt_opens_notepad"] = wait_for_log(
            "[FILES] opening in Notepad: appdata.txt", 25.0)
        checks["txt_not_executed"] = (
            "[FILES] running PE image appdata.txt" not in log_text())

        time.sleep(1.2)
        after_txt = load_ppm(grab(mon, "build/exedbl_after_txt.ppm"))
        drew_txt = diff_px(before_txt, after_txt, 380, 222, 900, 582)
        print(f"P2 pixels changed after appdata.txt double-click: {drew_txt}")
        checks["txt_notepad_drawn"] = drew_txt > 20000
        if shot:
            print("  shot:", png("build/exedbl_after_txt.ppm", "build/exedbl_after_txt.png"))

        slog = log_text()
        checks["no_fault"] = not (("TRIPLE FAULT" in slog) or ("PANIC" in slog)
                                  or ("EXCEPTION" in slog))

        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon:
                mon.sendall(b"quit\n")
        except OSError:
            pass
        time.sleep(0.5)
        qemu.kill()
        errf.close()


if __name__ == "__main__":
    sys.exit(main())
