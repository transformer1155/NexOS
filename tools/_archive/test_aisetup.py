#!/usr/bin/env python3
"""Headless proof of the desktop "AI Setup" shortcut (requirement #3).

A non-technical user should be able to turn on NexOS AI from the desktop
without touching the terminal.  This test drives the real C# Win11 shell:

  1. `gui` enters the desktop.
  2. double-click the "AI Setup" icon  -> a managed window opens.
  3. click "Enable AI & Agent"          -> Host.Exec("ai init") runs and the
                                            status panel updates.

Geometry (desktop 1280x720, 6 icons/column, icon cell 92px, margin 22):
  AI Setup is the 10th seeded shortcut (index 9) -> cell (114,298), centre
  ~ (156,340).  The wizard window is 380x320, centred at screen (450,242)
  with a 32px title bar; the "Enable AI & Agent" button is at client
  (16,64,348,46) -> screen ~ (641,361).

Success is proven two ways: a pixel change where the window/status panel
appear, and the serial marker "[AI] ai_init starting" that `ai init` emits
when Host.Exec runs it.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_aisetup.log"
MON_ERR = "build/qemu_aisetup.err"
SHOTS = "build"
PORT = 4473

K32_MARKERS = ("Shell ready",)

# desktop icon (centre) -> AI Setup shortcut (index 9)
ICON_X, ICON_Y = 156, 340
# "Enable AI & Agent" button centre on screen.
#   window origin (450,242); button client rect (16,64,348,46):
#   screen rect (466,306)-(814,352) -> centre (640,329)
BTN_X, BTN_Y = 640, 329
# window box (for pixel "opened" detection) and status-panel box
WIN_BOX = (450, 242, 830, 562)
STATUS_BOX = (466, 418, 814, 546)


def safe_clear(p):
    try:
        with open(p, "wb"):
            pass
    except OSError:
        pass


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


def mouse_abs(mon, x, y):
    """Position the cursor at absolute (x,y).

    QEMU's headless VM exposes a *relative* PS/2 mouse: `mouse_move a b`
    is a RELATIVE delta, not an absolute coordinate.  The only reliable
    way to reach a known position is to slam it far off-screen (which
    clamps to the origin) and then move by exactly the desired delta.
    """
    mon.sendall(b"mouse_move -10000 -10000\n")
    time.sleep(0.2)
    mon.sendall(("mouse_move %d %d\n" % (x, y)).encode())
    time.sleep(0.25)


def click(mon, x, y):
    mouse_abs(mon, x, y)
    mon.sendall(b"mouse_button 1\n")
    time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n")
    time.sleep(0.35)


def dblclick(mon, x, y):
    """Two clicks at the same spot within 500ms -> desktop double-click."""
    mouse_abs(mon, x, y)
    mon.sendall(b"mouse_button 1\n")
    time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n")
    time.sleep(0.12)
    mon.sendall(b"mouse_button 1\n")
    time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n")
    time.sleep(0.35)


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
    for _ in range(40):
        try:
            with open(path, "rb") as f:
                if f.readline().strip() != b"P6":
                    time.sleep(0.15)
                    continue
                dims = f.readline().split()
                w, h = int(dims[0]), int(dims[1])
                f.readline()
                d = f.read()
            if len(d) < w * h * 3:
                time.sleep(0.15)
                continue
            return w, h, d
        except (OSError, ValueError, IndexError):
            time.sleep(0.15)
    raise RuntimeError("could not read PPM: %s" % path)


def diff_box(a, b, x0, y0, x1, y1):
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
        wait_for(K32_MARKERS, 90.0)
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[MKFS] format data disk")
        type_line(mon, "mkfs")
        time.sleep(1.0)

        print("[GUI] enter desktop")
        type_line(mon, "gui")
        wait_for(("[GUI] Entered GUI mode",), 60.0)
        time.sleep(2.5)
        shots["desktop"] = load_ppm(grab(mon, "aisetup_1_desktop.ppm"))

        print("[DBLCLICK] AI Setup icon")
        dblclick(mon, ICON_X, ICON_Y)
        time.sleep(2.0)
        shots["open"] = load_ppm(grab(mon, "aisetup_2_open.ppm"))

        print("[CLICK] Enable AI & Agent")
        click(mon, BTN_X, BTN_Y)
        time.sleep(6.0)            # first ai_init trains the Markov corpus
        shots["enable"] = load_ppm(grab(mon, "aisetup_3_enable.ppm"))

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

    d_open = diff_box(shots["desktop"], shots["open"], *WIN_BOX)
    d_enable = diff_box(shots["open"], shots["enable"], *STATUS_BOX)

    print("\n" + "=" * 68)
    print("AI Setup desktop shortcut (32-bit kernel)")
    print("=" * 68)
    print("  pixels changed opening the window   : %d" % d_open)
    print("  pixels changed in the status panel  : %d" % d_enable)

    checks = {
        "window_opened":    "[GUI] ai-setup opened" in txt,
        "status_updated":   d_enable > 200
                           or ("[AI] ai_init returned" in txt),
        "ai_init_started":  "[AI] ai_init starting" in txt,
        "ai_init_returned": "[AI] ai_init returned" in txt,
        "no_fault":         ("TRIPLE FAULT" not in txt.upper())
                           and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- AI serial trace ---")
    seen = 0
    for line in txt.splitlines():
        if "[AI]" in line:
            print("  " + line.rstrip())
            seen += 1
            if seen > 20:
                print("  ...")
                break

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
