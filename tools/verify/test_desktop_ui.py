#!/usr/bin/env python3
"""End-to-end test for the desktop / file-manager UX round:
  1. Win32 menu API: winapp menu32.exe -> TrackPopupMenu session rendered
     in the GUI; clicking item 2 delivers WM_COMMAND(1002) to the window.
  2. Desktop icons open on DOUBLE-click, not single click.
  3. File Explorer: right-click a file -> context menu (Open/Edit/Open
     with.../Copy/Delete/Rename/Properties); "Open with..." -> sub-menu;
     Notepad opens the file.  Double-click also opens with Notepad.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_dui.img"
MPORT = 4481
SPORT = 4482

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
    logf = open("build/serial_dui.log", "ab")
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
    # QEMU monitor mouse_move is RELATIVE: run the pointer to (0,0) first
    # so move_to() can compute exact deltas.
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
    # First click: move there (slow relative-mouse home) then press.
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        # Second click: the pointer is ALREADY on the target, so press
        # again without move_to() -- that helper takes >1 s and would
        # blow the 500 ms double-click window.
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def rclick(mon, x, y):
    # QEMU monitor buttons: 1=left, 2=middle, 3=right.
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


def bright_in(d, x0, y0, x1, y1, thresh=200):
    """Count pixels brighter than thresh inside the box."""
    n = 0
    for y in range(y0, y1):
        ro = y * 1280 * 3
        for x in range(x0, x1):
            i = ro + x * 3
            if (d[i] + d[i + 1] + d[i + 2]) / 3 > thresh:
                n += 1
    return n


def diff_px(d1, d2, x0, y0, x1, y1, tol=12):
    """Count pixels that changed by > tol inside the box."""
    n = 0
    for y in range(y0, y1):
        r1 = y * 1280 * 3
        for x in range(x0, x1):
            i = r1 + x * 3
            a = abs(d1[i] - d2[i]) + abs(d1[i + 1] - d2[i + 1]) + abs(d1[i + 2] - d2[i + 2])
            if a > tol:
                n += 1
    return n


def nonblack_in(d, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        ro = y * 1280 * 3
        for x in range(x0, x1):
            i = ro + x * 3
            if d[i] or d[i + 1] or d[i + 2]:
                n += 1
    return n


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_dui.err", "wb")
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
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
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

        checks = {}

        # ============ Phase 1: Win32 menu API ==========================
        # winapp menu32.exe in text mode: builds a popup menu and calls
        # TrackPopupMenu (async session), reporting on the serial log.
        type_line(mon, "winapp menu32.exe")
        checks["w32_tracked"] = wait_for_log("[menu32] tracked popup menu", 40.0)
        print("P1 winapp tracked:", checks["w32_tracked"])
        time.sleep(1.0)

        # Enter the GUI: the compositor renders the Win32 popup at
        # (300,200).  Click item 2 "Edit" -> WM_COMMAND(1002).
        type_line(mon, "gui")
        checks["gui_enter"] = wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)
        click(mon, 330, 248)              # menu item 2 (index 1), row 2
        checks["w32_cmd"] = wait_for_log("[menu32] WM_COMMAND id=1002", 30.0)
        print("P1 w32 command:", checks["w32_cmd"])

        # ============ Phase 2: desktop double-click ====================
        # Calculator icon centre is (64,248).  Single click must NOT open.
        d_before = grab(mon, "build/dui_center_before.ppm")
        db = load_ppm(d_before)
        click(mon, 64, 248)               # single click -> select only
        time.sleep(0.9)
        d_single = grab(mon, "build/dui_center_single.ppm")
        ds = load_ppm(d_single)
        time.sleep(0.9)
        click(mon, 64, 248)               # second single (past 500 ms)
        time.sleep(0.9)
        d_single2 = grab(mon, "build/dui_center_single2.ppm")
        d2 = load_ppm(d_single2)
        # Window region (calculator would cover 530,282..750,522).
        diff_single = diff_px(db, ds, 400, 200, 880, 600)
        diff_single2 = diff_px(db, d2, 400, 200, 880, 600)
        print(f"P2 diff after single clicks: {diff_single} / {diff_single2}")
        checks["single_no_open"] = diff_single < 8000 and diff_single2 < 8000

        # Double-click opens the calculator.
        click(mon, 64, 248, dbl=True, gap=0.18)
        time.sleep(1.2)
        d_dbl = grab(mon, "build/dui_center_dbl.ppm")
        dd = load_ppm(d_dbl)
        diff_dbl = diff_px(d2, dd, 400, 200, 880, 600)
        print(f"P2 diff after double-click: {diff_dbl}")
        checks["dbl_opens"] = diff_dbl > 30000

        # Leave the GUI, clear the auto-saved session so Phase 3 starts
        # from a clean desktop, then re-enter.
        mon.sendall(b"sendkey esc\n"); time.sleep(0.8)
        wait_for_log("[GUI] Exited GUI mode", 30.0)
        type_line(mon, "session clear"); time.sleep(0.8)
        type_line(mon, "gui")
        wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)

        # ============ Phase 3: File Explorer ===========================
        # Double-click "This PC" (64,64) -> File Explorer window at
        # (380,222), client starts at (381,254).
        d_clean = grab(mon, "build/dui_clean.ppm")
        dcl = load_ppm(d_clean)
        click(mon, 64, 64, dbl=True, gap=0.18)
        time.sleep(1.5)
        d_fe = grab(mon, "build/dui_fe.ppm")
        df = load_ppm(d_fe)
        fe_diff = diff_px(dcl, df, 380, 222, 900, 582)
        print(f"P3 explorer diff: {fe_diff}")
        checks["fe_opened"] = fe_diff > 30000

        # Switch to System (SFS) volume: nav button centre (456,363).
        click(mon, 456, 363)
        time.sleep(0.8)

        # Right-click the first file row (781,319) -> context menu.
        rclick(mon, 781, 319)
        time.sleep(0.8)
        d_ctx = grab(mon, "build/dui_ctx.ppm")
        dc = load_ppm(d_ctx)
        # The menu is a light block near (781,319).
        ctx_bright = bright_in(dc, 660, 330, 960, 700)
        print(f"P3 context menu bright: {ctx_bright}")
        checks["ctx_menu"] = ctx_bright > 5000

        # "Open with..." is item index 3 -> y = 319 + 6 + 3*34 + 17 = 444.
        click(mon, 810, 444)
        time.sleep(0.8)
        d_sub = grab(mon, "build/dui_sub.ppm")
        dsu = load_ppm(d_sub)
        sub_bright = bright_in(dsu, 660, 330, 960, 700)
        print(f"P3 sub-menu bright: {sub_bright}")
        checks["sub_menu"] = sub_bright > 5000

        # Click "Notepad" (first row of the sub-menu at (789,327) ->
        # y = 327 + 6 + 17 = 350).  Notepad opens with the file loaded.
        click(mon, 810, 350)
        time.sleep(1.5)
        d_np = grab(mon, "build/dui_notepad.ppm")
        dn = load_ppm(d_np)
        # Notepad window at (400,222)..(880,582): compare to the sub-menu
        # frame (a big white editing surface appears).
        np_diff = diff_px(dsu, dn, 400, 222, 880, 582)
        print(f"P3 notepad diff: {np_diff}")
        checks["notepad_open"] = np_diff > 30000

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        checks["no_fault"] = not fault
        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
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
