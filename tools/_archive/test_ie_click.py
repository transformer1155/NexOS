#!/usr/bin/env python3
"""Headless proof that the built-in Internet Explorer actually responds to
the mouse and the keyboard.

The bug this guards
-------------------
iexplore.exe is a genuine PE32 with a genuine window procedure, and
gui.cpp was already translating desktop clicks into WM_LBUTTONDOWN /
WM_LBUTTONUP with a client-relative point, then repainting.  The window
procedure only handled WM_CREATE / WM_PAINT / WM_DESTROY, so every click
fell through DefWindowProcA and the page looked frozen.  Nothing in the
loader or the router was wrong -- nobody was listening.

What is asserted
----------------
  1. `gui browser` on the 32-bit kernel loads iexplore.exe and surfaces it.
  2. Clicking a favourite link repaints the document (pixels change) and
     the program says so on serial.
  3. Clicking "Back to the start page" restores the start page.
  4. Clicking the address bar focuses it, typing changes it and Enter
     navigates -- i.e. WM_CHAR / WM_KEYDOWN reach the program too.
  5. Clicking "Ask" runs the local AI engine through NexOS.dll and the
     answer panel changes.

Geometry
--------
launch_win32_windows() puts the PE's 880x540 client at desktop (40,40),
so the host window is at (40, 40+TOPBAR_H) = (40,72), 884x576, and
handle_app_click() takes the client origin as (x+2, content_y()+2) =
(42,106).  Every screen coordinate below is CLIENT + (42,106).
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_ieclick.log"
MON_ERR = "build/qemu_ieclick.err"
SHOTS = "build"
PORT = 4468

K32_MARKERS = ("Shell ready", "[K32]", "SFS:")

CLIENT_X, CLIENT_Y = 42, 106


def scr(cx, cy):
    """Client point -> screen point."""
    return CLIENT_X + cx, CLIENT_Y + cy


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            return True
        time.sleep(0.4)
    return False


KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
          '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
          '?': 'shift-slash', '=': 'equal'}


def send_key(mon, key, delay=0.08):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.07):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else KEYMAP.get(ch, ch)
        send_key(mon, key, delay)


def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.4)


STEP = 100


def _rel(mon, dx, dy):
    mon.sendall(("mouse_move %d %d\n" % (dx, dy)).encode())
    time.sleep(0.035)


def move_to(mon, x, y):
    # QEMU's PS/2 mouse is relative: slam the pointer into the top-left
    # corner first so the deltas below are absolute.
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.2)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.08)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.35)


def safe_clear(p):
    """Truncate a file in place.  Avoids os.remove(), which the Windows
    host intercepts via a safe-delete shim (recycle-bin) that can refuse
    to delete build artifacts."""
    try:
        with open(p, "wb") as fh:
            pass
    except OSError:
        pass


def grab(mon, name):
    path = os.path.join(SHOTS, name)
    if os.path.exists(path):
        safe_clear(path)
    mon.sendall(("screendump %s\n" % path).encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared: %s" % name)


def load_ppm(path):
    """Returns (w, h, bytes). Tolerates QEMU still writing the file."""
    for _ in range(40):
        try:
            with open(path, "rb") as f:
                if f.readline().strip() != b"P6":
                    time.sleep(0.15); continue
                dims = f.readline().split()
                w, h = int(dims[0]), int(dims[1])
                f.readline()
                d = f.read()
            if len(d) < w * h * 3:
                time.sleep(0.15); continue
            return w, h, d
        except (OSError, ValueError, IndexError):
            time.sleep(0.15)
    raise RuntimeError("could not read PPM: %s" % path)


def diff_box(a, b, x0, y0, x1, y1):
    """Number of differing pixels inside a screen-space box."""
    (aw, ah, ad), (bw, bh, bd) = a, b
    if (aw, ah) != (bw, bh):
        return -1
    n = 0
    for y in range(y0, min(y1, ah)):
        ro = y * aw * 3
        for x in range(x0, min(x1, aw)):
            i = ro + x * 3
            if ad[i] != bd[i] or ad[i + 1] != bd[i + 1] or ad[i + 2] != bd[i + 2]:
                n += 1
    return n


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        sys.exit(2)
    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            safe_clear(f)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    shots = {}
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for the 32-bit shell...")
        wait_for(K32_MARKERS, 45.0)
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] gui browser")
        type_line(mon, "gui browser")
        time.sleep(12.0)
        shots["start"] = load_ppm(grab(mon, "ie_1_start.ppm"))

        # --- 1. click the 4th favourite ("Help") -----------------------
        # start page: link i hit box is client (60, 264+i*24, 260, 22)
        print("[CLICK] favourite #3 (Help)")
        click(mon, *scr(190, 347))
        time.sleep(0.8)
        shots["doc"] = load_ppm(grab(mon, "ie_2_doc.ppm"))

        # --- 2. click "Back to the start page" -------------------------
        # doc page: client (60,236,220,20)
        print("[CLICK] back to the start page")
        click(mon, *scr(170, 246))
        time.sleep(0.8)
        shots["back"] = load_ppm(grab(mon, "ie_3_back.ppm"))

        # --- 3. focus the address bar, type, press Enter ---------------
        print("[CLICK] address bar, then type a URL")
        click(mon, *scr(500, 42))
        time.sleep(0.4)
        for _ in range(28):                       # clear the default URL
            send_key(mon, "backspace", 0.05)
        type_text(mon, "http://start.NexOS/downloads")
        time.sleep(0.4)
        shots["typed"] = load_ppm(grab(mon, "ie_4_typed.ppm"))
        send_key(mon, "ret", 0.9)
        shots["nav"] = load_ppm(grab(mon, "ie_5_nav.ppm"))

        # back home so the AI console is on screen again
        click(mon, *scr(170, 246))
        time.sleep(0.8)

        # --- 4. ask the local AI --------------------------------------
        print("[CLICK] AI question box, type, then Ask")
        click(mon, *scr(590, 280))
        time.sleep(0.4)
        type_text(mon, "what is NexOS")
        time.sleep(0.3)
        shots["preask"] = load_ppm(grab(mon, "ie_6_preask.ppm"))
        click(mon, *scr(784, 280))
        time.sleep(6.0)                            # first call boots the engine
        shots["asked"] = load_ppm(grab(mon, "ie_7_asked.ppm"))

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try:
                q.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                q.kill()
    errf.close()

    txt = read_log()

    # Document area in screen space: client (0,63)-(880,516).
    DX0, DY0 = scr(0, 63)
    DX1, DY1 = scr(880, 516)
    # The address bar strip, client (220,30)-(790,54).
    AX0, AY0 = scr(220, 30)
    AX1, AY1 = scr(790, 54)
    # The AI answer panel, client (440,302)-(820,454).
    PX0, PY0 = scr(440, 302)
    PX1, PY1 = scr(820, 454)

    d_open = diff_box(shots["start"], shots["doc"], DX0, DY0, DX1, DY1)
    d_back = diff_box(shots["doc"], shots["back"], DX0, DY0, DX1, DY1)
    d_type = diff_box(shots["start"], shots["typed"], AX0, AY0, AX1, AY1)
    d_nav = diff_box(shots["typed"], shots["nav"], DX0, DY0, DX1, DY1)
    d_ask = diff_box(shots["preask"], shots["asked"], PX0, PY0, PX1, PY1)
    same_home = diff_box(shots["start"], shots["back"], DX0, DY0, DX1, DY1)

    print("\n" + "=" * 68)
    print("Internet Explorer (PE32 i386): mouse + keyboard interactivity")
    print("=" * 68)
    print("  pixels changed opening a favourite     : %d" % d_open)
    print("  pixels changed going back              : %d" % d_back)
    print("  pixels changed typing in the address   : %d" % d_type)
    print("  pixels changed navigating (Enter)      : %d" % d_nav)
    print("  pixels changed in the AI answer panel  : %d" % d_ask)
    print("  start-page vs after-back difference    : %d  (want ~0)" % same_home)

    checks = {
        "browser_launched":   "[app] [iexplore] MiniPE browser starting" in txt,
        "window_surfaced":    "iexplore.exe surfaced" in txt,
        "click_reached_pe":   "[app] [iexplore] WM_LBUTTONDOWN handled" in txt,
        "link_opened_page":   d_open > 500,
        "back_repainted":     d_back > 500,
        "back_restored_home": 0 <= same_home < 400,
        "typing_reached_pe":  d_type > 50,
        "enter_navigated":    d_nav > 300,
        "ai_answered":        "[app] [iexplore] AI answered" in txt,
        "ai_panel_repainted": d_ask > 200,
        "paint_budget_ok":    "WARNING display list overflowed" not in txt,
        "no_fault":           ("TRIPLE FAULT" not in txt.upper())
                              and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- iexplore serial trace ---")
    seen = 0
    for line in txt.splitlines():
        if "[iexplore]" in line or "[AI]" in line:
            print("  " + line.rstrip())
            seen += 1
            if seen > 40:
                print("  ...")
                break

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
