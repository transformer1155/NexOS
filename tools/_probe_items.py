#!/usr/bin/env python3
"""Click every item of every context menu; report menu-close + visible effect.

The user reports that some right-click menu items "do nothing" and the menu
sometimes "does not close".  Menu-opening is already verified, so this probe
exercises the ITEM-CLICK path: for each menu it right-clicks to open, clicks
every non-separator item, screendumps after each click and classifies:

  - menu closed?        diff inside the menu rect between before/after
  - visible effect?     diff vs the clean desktop, outside the menu rect
  - hit-uncertain?      no dark label ink at the item's text region in the
                        "before" shot (geometry/state mismatch)

Menu geometry is computed with the SAME math as NexOS.Forms.Popup.Open
(n items, ItemH=34, PadY=6, min width 150, clamped to the 1280x720 screen),
so it cannot be fooled by a bright wallpaper the way pixel-band detection was.

Every item is clicked against a FRESH menu (reopen per item), and side
effects are managed: windows opened by an item (Task Manager, Control Panel,
File Explorer, Notepad) are closed via their title-bar X button so later
scenarios still see a clean surface.  The Terminal shortcuts hand control
back to the text shell (kernel design), so that item is clicked last.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG  = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/probe_items_work.img"
LOG  = "build/serial_probe_items.log"
RES  = "build/probe_items_result.txt"
ERR  = "build/qemu_probe_items.err"
PORT = 4463

SCREEN_W, SCREEN_H = 1280, 720
BOTTOM   = SCREEN_H - 4
ITEMH    = 34
PADY     = 6
DARK     = 250          # RGB sum below this counts as dark label ink
AREA_FRAC_MENU_OPEN = 0.45
VIS_HI, VIS_WEAK = 15000, 4000
TASKBAR_Y = 660         # y>=660 (taskbar strip) excluded from "visible" diff


def mark(s):
    try:
        with open(RES, "a") as f:
            f.write(s + "\n")
    except Exception:
        pass


def wait_for_serial(pattern, timeout=60.0):
    """Poll the serial log until it contains `pattern`."""
    end = time.time() + timeout
    seen = b""
    while time.time() < end:
        try:
            with open(LOG, "rb") as f:
                seen = f.read()
            if pattern.encode("latin-1") in seen:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


# ---- QEMU monitor --------------------------------------------------
def wait_sock(port, timeout=40.0):
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


def dump(mon, name):
    try:
        os.remove(f"build/{name}.ppm")
    except OSError:
        pass
    mon.sendall(f"screendump build/{name}.ppm\n".encode())
    time.sleep(1.2)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px


def read_ppm_retry(path, tries=15):
    last = None
    for i in range(tries):
        try:
            with open(path, "rb") as f:
                assert f.readline().strip() == b"P6"
                w, h = (int(x) for x in f.readline().split())
                f.readline()                     # maxval line
                px = f.read()
            expect = w * h * 3
            if len(px) >= expect and os.path.getsize(path) >= 15 + expect:
                return w, h, px[:expect]
        except (IndexError, AssertionError, OSError, ValueError) as e:
            last = e
        time.sleep(0.4)
    if last is not None:
        raise last
    return read_ppm(path)


def region_pixel_diff(path_a, path_b, x0, y0, rw, rh, tol=24, stride=1):
    w, h, pa = read_ppm_retry(path_a)
    _, _, pb = read_ppm_retry(path_b)
    x0 = max(0, min(x0, w - 1)); y0 = max(0, min(y0, h - 1))
    x1 = min(w, x0 + rw);        y1 = min(h, y0 + rh)
    n = 0
    for yy in range(y0, y1, stride):
        row = yy * w * 3
        for xx in range(x0, x1, stride):
            i = row + xx * 3
            if (abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1])
                    + abs(pa[i + 2] - pb[i + 2])) > tol:
                n += 1
    return n


def dark_in_rect(path, x0, y0, rw, rh):
    """Count pixels darker than DARK inside the rect (label-ink check)."""
    w, h, px = read_ppm_retry(path)
    x0 = max(0, min(x0, w - 1)); y0 = max(0, min(y0, h - 1))
    x1 = min(w, x0 + rw);        y1 = min(h, y0 + rh)
    n = 0
    for yy in range(y0, y1, 2):
        row = yy * w * 3
        for xx in range(x0, x1, 2):
            i = row + xx * 3
            if px[i] + px[i + 1] + px[i + 2] < DARK:
                n += 1
    return n


# ---- mouse (small tracked relative moves; big slams corrupt QEMU) ----
_CURSOR = [640, 360]


def mouse_abs(mon, x, y):
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx))
        sy = max(-80, min(80, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.1)
        _CURSOR[0] += sx
        _CURSOR[1] += sy
        dx -= sx
        dy -= sy
    time.sleep(0.2)


def mouse_press(mon, btn):
    mon.sendall(("mouse_button %d\n" % btn).encode())
    time.sleep(0.15)


def left_click(mon, x, y):
    mouse_abs(mon, x, y)
    mouse_press(mon, 1)
    time.sleep(0.1)
    mouse_press(mon, 0)
    time.sleep(0.25)


def right_click(mon, x, y):
    mouse_abs(mon, x, y)
    mouse_press(mon, 2)
    time.sleep(0.6)
    mouse_press(mon, 0)
    time.sleep(0.3)


def open_menu(mon, x, y):
    left_click(mon, 5, 5)          # dismiss any lingering menu / popup
    time.sleep(0.4)
    right_click(mon, x, y)


# ---- geometry (mirrors NexOS.Forms.Popup.Open) ---------------------
def text_w(s):
    return sum(8 for ch in s)      # Gfx.Measure: 8px per ASCII glyph


def menu_rect(rx, ry, labels, n):
    mw = 0
    for i in range(n):
        if labels[i]:
            tw = text_w(labels[i])
            if tw > mw:
                mw = tw
    w = max(150, mw + 40)
    h = n * ITEMH + PADY * 2
    x = max(4, min(rx, SCREEN_W - 4 - w))
    y = max(4, min(ry, BOTTOM - h))
    return (x, y, w, h)


def item_center(rect, i):
    x, y, w, h = rect
    return (x + w // 2, y + PADY + i * ITEMH + ITEMH // 2)


# Title-bar X button of windows gui.cpp opens (only one window is ever
# open at a time in this probe, closed right after each item that spawns it).
WCLOSE = {
    0: (884, 198),   # Control Panel   520x440 @ (380,182)
    1: (884, 238),   # File Explorer   520x360 @ (380,222)
    2: (904, 208),   # Task Manager    560x420 @ (360,192)
    7: (864, 238),   # Notepad         480x360 @ (400,222)
}


def close_window(mon, kind):
    if kind not in WCLOSE:
        return
    x, y = WCLOSE[kind]
    left_click(mon, x, y)
    time.sleep(0.4)


# ---- scenario runner -------------------------------------------------
def click_item(mon, tag, rect, i, label, clean, expect_vis):
    x, y, w, h = rect
    cx, cy = item_center(rect, i)
    before = f"build/pi_{tag}_before.ppm"
    after = f"build/pi_{tag}_after.ppm"
    # hit sanity: dark label ink must sit where the item's text should be
    tw = min(text_w(label), 96)
    dk = dark_in_rect(before, x + 14, y + PADY + i * ITEMH + 8, tw + 4, 20)
    hit = "" if dk >= 3 else " HIT-UNSURE"
    mouse_abs(mon, cx, cy)
    time.sleep(0.2)
    mouse_press(mon, 1)
    time.sleep(0.1)
    mouse_press(mon, 0)
    time.sleep(0.5)
    mouse_abs(mon, 5, 5)
    time.sleep(0.3)
    dump(mon, f"pi_{tag}_after")
    area = w * h
    d_menu = region_pixel_diff(before, after, x, y, w, h)
    closed = d_menu > area * AREA_FRAC_MENU_OPEN
    state = "CLOSED" if closed else "MENU-STAYS-OPEN"
    d_vis = 0
    if clean:
        cw, ch, _ = read_ppm_retry(clean)
        top = min(ch, TASKBAR_Y)
        d_vis = region_pixel_diff(clean, after, 0, 0, cw, top, stride=2)
        mh = min(h, top - y) if y < top else 0
        if mh > 0:
            d_vis -= region_pixel_diff(clean, after, x, y, w, mh)
        if d_vis < 0:
            d_vis = 0
    vis = ("VISIBLE" if d_vis > VIS_HI else
           "weak" if d_vis > VIS_WEAK else "none")
    note = ""
    if expect_vis == "none" and vis == "none":
        note = " (expected)"
    elif expect_vis == "visible" and vis != "none":
        note = " (ok)"
    elif expect_vis == "visible" and vis == "none":
        note = " <-- EXPECTED VISIBLE"
    msg = (f"{label}: {state} vis={vis}{note} (dmenu={d_menu}/{area} "
           f"dvis={d_vis}){hit}")
    print("  " + msg)
    mark(f"{tag} item {i} {label}: {state} vis={vis}{note}{hit}")
    return closed


def run_menu(mon, tag, name, rx, ry, labels, items, clean, expect):
    """Reopen the menu for every item and click it fresh."""
    print(f"--- {name} (right-click {rx},{ry}) ---")
    for i, label in items:
        sub = tag if len(items) == 1 else f"{tag}_{i}"
        open_menu(mon, rx, ry)
        dump(mon, f"pi_{sub}_before")
        rect = menu_rect(rx, ry, labels, len(labels))
        dk = dark_in_rect(f"build/pi_{sub}_before.ppm",
                          rect[0] + 14, rect[1] + PADY + 8, 200,
                          max(10, rect[3] - 12))
        if dk < 3:
            print(f"  !! item {i}: no menu detected at {rect} (dark px {dk})")
            mark(f"{tag} item {i} {label}: NO-MENU-DETECTED")
            continue
        exp = "visible" if i in expect and expect[i] == "visible" else "none"
        click_item(mon, sub, rect, i, label, clean, exp)


def open_with_submenu(mon, tag, rx, ry, clean, sub_actions):
    """Open the file menu, click 'Open with...' and act on the submenu."""
    icon_labels = ["Open", "Edit", "Open with Terminal", "Open with...",
                   "", "Copy", "Delete", "Rename", "Properties", "",
                   "New folder"]
    print(f"--- {tag}: Open with... submenu (right-click {rx},{ry}) ---")
    for i, label, exp in sub_actions:
        open_menu(mon, rx, ry)
        dump(mon, f"pi_{tag}_ow_before")
        rect = menu_rect(rx, ry, icon_labels, 11)
        dk = dark_in_rect(f"build/pi_{tag}_ow_before.ppm",
                          rect[0] + 14, rect[1] + PADY + 8, 200, rect[3] - 12)
        if dk < 3:
            print(f"  !! no main menu at {rect} (dark px {dk})")
            mark(f"{tag}: NO-MENU-DETECTED")
            continue
        click_item(mon, f"{tag}_ow", rect, 3, "Open with...", clean, "none")
        time.sleep(0.5)
        dump(mon, f"pi_{tag}_sub{i}_before")
        sub = menu_rect(rx + 8, ry + 8, ["Notepad", "Terminal"], 2)
        dk2 = dark_in_rect(f"build/pi_{tag}_sub{i}_before.ppm",
                           sub[0] + 14, sub[1] + PADY + 8, 200, sub[3] - 12)
        if dk2 < 3:
            print(f"  !! no submenu at {sub} (dark px {dk2})")
            mark(f"{tag} submenu: NO-BBOX")
            continue
        print(f"  submenu rect={sub}")
        click_item(mon, f"{tag}_sub{i}", sub, i, label, clean, exp)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (RES, LOG):
        if os.path.exists(f):
            os.remove(f)
    errf = open(ERR, "wb")
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
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        print("booting...")
        if not wait_for_serial("[K] Command-line shell", 60.0):
            print("!! boot shell prompt not seen in serial log")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        type_line(mon, "gui")
        if not wait_for_serial("[MFORMS] managed shell ready", 60.0):
            print("!! [MFORMS] ready not seen - GUI may not have started")
        time.sleep(2.0)
        print("[gui] desktop up")

        left_click(mon, 5, 5)
        time.sleep(0.5)
        dump(mon, "pi_clean0")

        # ---- tray menus -------------------------------------------
        run_menu(mon, "tray_voice", "tray voice", 1155, 695,
                 ["Turn on voice input"], [(0, "Turn on voice input")],
                 "build/pi_clean0.ppm", {})

        run_menu(mon, "tray_tasks", "tray tasks", 1115, 695,
                 ["", "Open Task Manager"], [(1, "Open Task Manager")],
                 "build/pi_clean0.ppm", {1: "visible"})
        close_window(mon, 2)

        run_menu(mon, "tray_net", "tray network", 1193, 695,
                 ["Ethernet", "Wi-Fi", "Network settings"],
                 [(0, "Ethernet"), (1, "Wi-Fi"), (2, "Network settings")],
                 "build/pi_clean0.ppm", {2: "visible"})
        close_window(mon, 0)

        # ---- taskbar menus ----------------------------------------
        run_menu(mon, "pin", "taskbar pin (File Explorer)", 505, 690,
                 ["Close window", "End process"],
                 [(0, "Close window"), (1, "End process")],
                 "build/pi_clean0.ppm", {})

        run_menu(mon, "bar", "taskbar empty", 900, 690,
                 ["Task Manager", "Taskbar settings"],
                 [(0, "Task Manager"), (1, "Taskbar settings")],
                 "build/pi_clean0.ppm", {0: "visible", 1: "visible"})
        close_window(mon, 2)
        close_window(mon, 0)

        # ---- desktop icon (This PC) file menu ---------------------
        icon_labels = ["Open", "Edit", "Open with Terminal", "Open with...",
                       "", "Copy", "Delete", "Rename", "Properties", "",
                       "New folder"]
        run_menu(mon, "icon", "desktop icon 'This PC' (68,68)", 68, 68,
                 icon_labels,
                 [(0, "Open"), (1, "Edit"), (5, "Copy"),
                  (7, "Rename"), (8, "Properties")],
                 "build/pi_clean0.ppm", {0: "visible", 1: "visible"})
        close_window(mon, 1)      # File Explorer from Open/Edit
        mon.sendall(b"sendkey ret\n")    # commit (unchanged) inline rename
        time.sleep(0.4)
        left_click(mon, 5, 5)            # dismiss the properties popup
        time.sleep(0.3)

        # Open with... -> Notepad
        open_with_submenu(mon, "icon", 68, 68, "build/pi_clean0.ppm",
                          [(0, "sub Notepad", "visible")])
        close_window(mon, 7)      # Notepad

        # Delete on a different icon (Calculator) so This PC survives
        run_menu(mon, "icon_del", "desktop icon 'Calculator' (68,252)", 68, 252,
                 icon_labels, [(6, "Delete")],
                 "build/pi_clean0.ppm", {6: "visible"})

        # New folder on This PC (last icon action; may re-order icons)
        run_menu(mon, "icon_mk", "desktop icon 'This PC' New folder", 68, 68,
                 icon_labels, [(10, "New folder")],
                 "build/pi_clean0.ppm", {10: "visible"})

        # ---- desktop blank-area menu ------------------------------
        run_menu(mon, "desk", "desktop wallpaper (640,400)", 640, 400,
                 ["Sort by name", "Sort by size", "Sort by type",
                  "Sort by date modified", "", "Refresh", "Personalize",
                  "Open in terminal", "New folder"],
                 [(0, "Sort by name"), (1, "Sort by size"),
                  (2, "Sort by type"), (3, "Sort by date modified"),
                  (5, "Refresh"), (8, "New folder"), (6, "Personalize")],
                 "build/pi_clean0.ppm", {6: "visible"})
        close_window(mon, 0)      # Control Panel from Personalize

        # ---- Terminal shortcuts end the run (kernel: gui_exit) ----
        print("--- [final] desktop 'Open in terminal' (exits GUI) ---")
        open_menu(mon, 640, 400)
        dump(mon, "pi_term_before")
        rect = menu_rect(640, 400,
                         ["Sort by name", "Sort by size", "Sort by type",
                          "Sort by date modified", "", "Refresh",
                          "Personalize", "Open in terminal", "New folder"], 9)
        dk = dark_in_rect("build/pi_term_before.ppm",
                          rect[0] + 14, rect[1] + PADY + 8, 200, rect[3] - 12)
        if dk >= 3:
            click_item(mon, "term", rect, 7, "Open in terminal",
                       "build/pi_clean0.ppm", "visible")
        else:
            print(f"  !! final menu not detected at {rect}")
            mark("term item 7 Open in terminal: NO-MENU-DETECTED")
        time.sleep(1.5)

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    data = open(LOG, "rb").read().decode("latin-1", "ignore")
    print("\n--- serial: faults/exceptions ---")
    found = 0
    for ln in data.splitlines():
        if ("CLR] fault" in ln or "PANIC" in ln or "TRIPLE" in ln
                or "EXCEPTION" in ln):
            print("  " + ln)
            found += 1
    if not found:
        print("  (none)")
    print("--- serial tail ---")
    print("\n".join(data.splitlines()[-12:]))


if __name__ == "__main__":
    main()
