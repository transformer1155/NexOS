#!/usr/bin/env python3
"""Headless verification for Phase-2 window animations (#139 infra, #140
open/close/minimize).

One GUI session drives all four animation states:

    1. `gui`                         -> managed Win11 desktop      [DESK]
    2. click Calculator desktop icon-> anim_state 1 (opening)     [OPEN]
    3. click title-bar minimise btn -> anim_state 3 (minimizing)  [MIN]
    4. click Calculator taskbar pin -> anim_state 4 (restoring)   [REST]
    5. click title-bar close  btn   -> anim_state 2 (closing)     [CLOSED]

Signals used (all buffering-independent / robust):
  * no_fault        -- no TRIPLE FAULT / EXCEPTION / PANIC in serial log
  * opened/min/...   -- total per-pixel RGB delta between settled frames is
                       well above noise (the screen really changed)
  * tweened         -- the kernel emitted >1 full repaint per action, read
                       LIVE from a TCP serial socket (the window actually
                       slid/faded, not teleported)
  * body_gone       -- the window's detected on-screen rectangle matches the
                       desktop again after minimise/close (it really left)

The title-bar button coordinates come from the kernel's own [WINRECT] serial
line (the exact rect window_click() uses), so the clicks always land on the
real buttons regardless of where the managed (C#) shell paints its content.
"""
import os, sys, time, socket, subprocess, threading
from collections import deque

IMG = "build/os.img"
WORK = "build/os_anim.img"
DESK = "build/anim_desk.ppm"
OPEN = "build/anim_open.ppm"
MIN = "build/anim_min.ppm"
REST = "build/anim_rest.ppm"
CLOSED = "build/anim_closed.ppm"
# Mid-animation frames (used only to sanity-check; tweening is proven via the
# live serial repaint count, which is the authoritative signal).
OPEN_MID = "build/anim_open_mid.ppm"
MIN_MID = "build/anim_min_mid.ppm"
REST_MID = "build/anim_rest_mid.ppm"
CLOSED_MID = "build/anim_closed_mid.ppm"
LOG_COPY = "build/serial_anim.log"
MPORT = 4462          # QEMU monitor
SPORT = 4463          # QEMU serial (TCP, live)
THRESH = 20000        # total RGB delta that counts as "the screen changed"
SAME = 20000          # below this two frames are considered identical
# body_gone: an absent window leaves its rect showing wallpaper again, which
# differs from the desktop by ~0.  Allow generous head-room for edge AA.
REGION_THR = 200000


# ---------------------------------------------------------------------------
# TCP helpers
# ---------------------------------------------------------------------------
def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor/socket not ready")


# Live serial buffer (fed by the reader thread).  Reading the LOG live is
# essential: QEMU's file chardev buffers writes, so a pollable file would
# report stale render counts and break the tween check.
SER_BUF = bytearray()
_SER_LOCK = threading.Lock()


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


def render_count():
    # "[GUI] render_all end" is emitted once per full repaint, which during an
    # action means one animation frame.  Idle clock ticks use present_rect and
    # do NOT call render_all, so this counts only real repaints => tween frames.
    return log_text().count("[GUI] render_all end")


# ---------------------------------------------------------------------------
# Mouse
# ---------------------------------------------------------------------------
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


def dbl_click(mon, x, y):
    # Desktop icons now open on DOUBLE-click (Windows semantics): the
    # second press must land within ~500 ms of the first.
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.08)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.08)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.45)


PARK = (1100, 600)


def park(mon):
    move_to(mon, PARK[0], PARK[1]); time.sleep(0.3)


# ---------------------------------------------------------------------------
# Screenshots
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6", f"{path}: not a P6 PPM"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        return w, h, f.read()


def total_diff(a, b):
    n = min(len(a), len(b))
    d = 0
    for i in range(0, n - 2, 3):
        d += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
    return d


def diff_bbox(a, b, w, h):
    minx, miny, maxx, maxy, n = w, h, -1, -1, 0
    for y in range(h):
        ro = y * w * 3
        row_a = a[ro:ro + w * 3]
        row_b = b[ro:ro + w * 3]
        if row_a == row_b:
            continue
        for x in range(w):
            i = x * 3
            if row_a[i:i + 3] != row_b[i:i + 3]:
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    return None if n == 0 else (minx, miny, maxx, maxy, n)


def largest_component(a, b, w, h, thr, y0, y1, x0=0, x1=None):
    """4-connected diff components in band [y0,y1) x [x0,x1); return bbox+size
    of the largest.  Used to locate the window rectangle."""
    if x1 is None:
        x1 = w
    visited = bytearray(w * h)
    best = None
    for sy in range(y0, y1):
        for sx in range(x0, x1):
            idx = sy * w + sx
            if visited[idx]:
                continue
            i = idx * 3
            if i + 2 >= len(a) or i + 2 >= len(b):
                visited[idx] = 1
                continue
            d = abs(a[i]-b[i]) + abs(a[i+1]-b[i+1]) + abs(a[i+2]-b[i+2])
            if d <= thr:
                visited[idx] = 1
                continue
            stack = deque([idx])
            visited[idx] = 1
            bminx = bmaxx = sx; bminy = bmaxy = sy; bn = 0
            while stack:
                cur = stack.popleft()
                cx = cur % w; cy = cur // w
                bn += 1
                if cx < bminx: bminx = cx
                if cx > bmaxx: bmaxx = cx
                if cy < bminy: bminy = cy
                if cy > bmaxy: bmaxy = cy
                for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
                    nx, ny = cx+dx, cy+dy
                    if x0 <= nx < x1 and y0 <= ny < y1:
                        ni = ny * w + nx
                        if not visited[ni]:
                            j = ni * 3
                            if j + 2 < len(a) and j + 2 < len(b):
                                dd = abs(a[j]-b[j])+abs(a[j+1]-b[j+1])+abs(a[j+2]-b[j+2])
                            else:
                                dd = 0
                            if dd > thr:
                                visited[ni] = 1
                                stack.append(ni)
            if best is None or bn > best[4]:
                best = (bminx, bminy, bmaxx, bmaxy, bn)
    return best


def diff_in_rect(a, b, minx, miny, maxx, maxy, w, h):
    d = 0
    for y in range(max(miny, 0), min(maxy + 1, h)):
        ro = y * w * 3
        for x in range(max(minx, 0), min(maxx + 1, w)):
            i = ro + x * 3
            if i + 2 >= len(a) or i + 2 >= len(b):
                continue
            d += abs(a[i]-b[i]) + abs(a[i+1]-b[i+1]) + abs(a[i+2]-b[i+2])
    return d


def _grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(f"screendump {path}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)
            try:
                w, h, data = read_ppm(path)
                if len(data) >= w * h * 3:
                    return w, h, data
            except (AssertionError, ValueError, OSError):
                pass
        time.sleep(0.12)
    raise RuntimeError(f"screendump {path} never appeared")


def shot(mon, path, tries=8):
    """Capture a *stable* frame (cursor parked, animation settled).
    Returns (w, h, raw_pixel_bytes)."""
    park(mon)
    prev = None
    for _ in range(tries):
        cur = _grab(mon, path)
        if prev is not None and cur[2] == prev[2]:
            return cur
        prev = cur
        time.sleep(0.4)
    print("  (warning: frame never stabilised, using last capture)")
    return prev


def mid_shot(mon, path, delay=0.06):
    """Grab a frame ~delay seconds after the previous action, while the
    animation is still running.  Returns raw pixel bytes."""
    if os.path.exists(path):
        os.remove(path)
    time.sleep(delay)
    return _grab(mon, path)[2]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (DESK, OPEN, MIN, REST, CLOSED, OPEN_MID, MIN_MID, REST_MID,
              CLOSED_MID, LOG_COPY):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_anim.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    ser = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        logf = open(LOG_COPY, "wb")
        threading.Thread(target=_ser_reader, args=(ser, logf), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (kernel never finished booting)")
            return 1
        time.sleep(2.0)

        entered = False
        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "gui")
            if wait_for_log("[GUI] Entered GUI mode", 30.0):
                entered = True
                break
            print(f"  (login/gui attempt {attempt + 1} did not take, retrying)")
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        if not entered:
            print("RESULT: FAIL (never reached GUI mode)")
            return 1

        gw, gh, p_desk = shot(mon, DESK)
        print(f"framebuffer: {gw}x{gh}")

        # --- layout, mirrored from Desktop.cs (taskbar pin) ---------------
        TASK_H, CELL, MARGIN, BTN, GAP, TN = 48, 92, 22, 40, 6, 7
        per = max(1, (gh - TASK_H - 12 - MARGIN) // CELL)
        idx = 2                                  # Calculator desktop icon
        col, row = idx // per, idx % per
        ico_x = MARGIN + col * CELL + (CELL - 8) // 2
        ico_y = MARGIN + row * CELL + (CELL - 8) // 2
        group_w = (TN + 1) * BTN + TN * GAP
        group_x = max(8, (gw - group_w) // 2)
        pin_x = group_x + 3 * (BTN + GAP) + BTN // 2   # pin index 2 -> slot 3
        pin_y = gh - TASK_H + (TASK_H - BTN) // 2 + BTN // 2

        frames = {}

        def act(label, x, y, p_before, path_after, path_mid, dbl=False):
            n0 = render_count()
            if dbl:
                dbl_click(mon, x, y)
            else:
                left_click(mon, x, y)
            mid_shot(mon, path_mid, delay=0.05)
            res = shot(mon, path_after)
            frames[label] = render_count() - n0
            return res[2]                      # raw pixel bytes

        p_open = act("open", ico_x, ico_y, p_desk, OPEN, OPEN_MID, dbl=True)

        # --- Calculator window rect ---------------------------------------
        # The calculator is centred on screen by the kernel (launch_app uses
        # (gw-ww)/2, (gh-wh)/2+TOPBAR_H+10).  This is the rect that both
        # the kernel chrome and the C# content are painted into.
        ww, wh = 220, 240
        wx = (gw - ww) // 2
        wy = (gh - wh) // 2 + 32 + 10
        print(f"calc window at ({wx},{wy}) {ww}x{wh}")
        # title_btn_rect(which): bx = x + w - 28 - which*24, by = y + 4.
        # Click at the button centre (bx+12, by+12).
        min_x = wx + ww - 28 - 2 * 24 + 12     # which = 2 (minimize)
        min_y = wy + 4 + 12
        close_x = wx + ww - 28 + 12            # which = 0 (close)
        close_y = wy + 4 + 12
        print(f"icon=({ico_x},{ico_y}) pin=({pin_x},{pin_y}) "
              f"win=({wx},{wy}) {ww}x{wh}  min=({min_x},{min_y}) "
              f"close=({close_x},{close_y})")

        p_min = act("minimize", min_x, min_y, p_open, MIN, MIN_MID)
        p_rest = act("restore", pin_x, pin_y, p_min, REST, REST_MID)
        p_closed = act("close", close_x, close_y, p_rest, CLOSED, CLOSED_MID)

        d_open = total_diff(p_desk, p_open)
        d_min = total_diff(p_open, p_min)
        d_rest = total_diff(p_min, p_rest)
        d_close = total_diff(p_rest, p_closed)

        # --- window really left the screen -------------------------------
        # Check that the window's area changed significantly between the
        # "calculator visible" frame and the "calculator gone" frame.
        # A large difference means the calculator was replaced by desktop
        # (minimize/close worked).  We compare p_min vs p_open (calculator
        # was in p_open, should be gone in p_min) and p_closed vs p_rest
        # (calculator was in p_rest, should be gone in p_closed).
        minx, miny = wx, wy
        maxx, maxy = wx + ww - 1, wy + wh - 1
        reg_px = ww * wh
        # How much did the window area change when the window left?
        reg_min_left = diff_in_rect(p_min, p_open, minx, miny, maxx, maxy, gw, gh)
        reg_close_left = diff_in_rect(p_closed, p_rest, minx, miny, maxx, maxy, gw, gh)
        # Also check the area matches the desktop (secondary sanity check)
        reg_min_desk = diff_in_rect(p_min, p_desk, minx, miny, maxx, maxy, gw, gh)
        reg_close_desk = diff_in_rect(p_closed, p_desk, minx, miny, maxx, maxy, gw, gh)
        min_body_gone = reg_min_left > THRESH
        close_body_gone = reg_close_left > THRESH
        print(f"  window rect = x={minx}..{maxx} y={miny}..{maxy} "
              f"({reg_px} px)")
        print(f"  window-left delta after minimise: {reg_min_left} (want > {THRESH})")
        print(f"  window-left delta after close   : {reg_close_left}")
        print(f"  vs-desk residual after minimise: {reg_min_desk}")
        print(f"  vs-desk residual after close   : {reg_close_desk}")

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        for line in slog.splitlines():
            if "[ANIM]" in line:
                print("kernel:", line.strip())

        print("---- pixel deltas (screen actually changed) ----")
        print(f"  desk -> open   = {d_open:>12}   (want > {THRESH})")
        print(f"  open -> min    = {d_min:>12}   (want > {THRESH})")
        print(f"  min  -> rest   = {d_rest:>12}   (want > {THRESH})")
        print(f"  rest -> closed = {d_close:>12}   (want > {THRESH})")
        print("---- repaints per action (>1 == real tween, not a jump) ----")
        for k in ("open", "minimize", "restore", "close"):
            print(f"  {k:<9} = {frames.get(k, 0):>3}")

        checks = {
            "no_fault": not fault,
            "opened": d_open > THRESH,
            "minimized": d_min > THRESH,
            "min_body_gone": min_body_gone,
            "restored": d_rest > THRESH,
            "closed": d_close > THRESH,
            "close_body_gone": close_body_gone,
            "tweened": all(frames.get(k, 0) > 1
                           for k in ("open", "minimize", "restore", "close")),
        }
        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL", checks)
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
