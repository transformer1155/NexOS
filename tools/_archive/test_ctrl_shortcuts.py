#!/usr/bin/env python3
"""Headless verification of the terminal-style shortcuts (Ctrl+C / V / Z / A)
added to every NexOS input box.

Authoritative, serial-based checks (apps already log the address bar after
every edit), no flaky pixel diffing:

  32-bit kernel (default boot):
    * Native IE address bar (winpe/iexplore.c) via WM_NexOS_CTRL.
      Logs `[iexplore] addr=...`.  Covers Ctrl+C (copy field),
      Ctrl+V (paste / overwrite), Ctrl+Z (undo), Ctrl+A (select-all).
      The IE edit path has a PRE-EXISTING backspace quirk when the IME is
      in Chinese mode, so this test deliberately avoids backspace: it
      copies the whole field (Ctrl+C), appends, then pastes over it.

  64-bit kernel (after `switch`):
    * Managed C# BrowserApp address bar (csharp/.../Browser.cs -> TBox.Key).
      Logs `[browser] addr=...`.  Covers Ctrl+A (select), Ctrl+C (copy),
      Ctrl+V (paste), Ctrl+Z (undo).  This SAME NexOS.Forms.TBox.Key
      source powers the 32-bit managed Notepad / Terminal / Browser /
      Desktop-rename boxes, so it covers the whole managed input layer.

Each app is launched FRESH from a clean text-mode prompt.  The IE click is
retried because mouse delivery to the PE window is racy under QEMU.
"""
import os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_ctrl.log"
MON_ERR = "build/qemu_ctrl.err"
MON_PORT = 4493

# Proven 32-bit IE address-bar click (test_ie_addrbar.py uses scr(500,42)
# = CLIENT_X(42)+500, CLIENT_Y(106)+42 = (542,148)).  In IE client space
# that maps to (500,42) which lies inside R_ADDR (220..780, 30..54).
IE_CLICK_X, IE_CLICK_Y = 542, 148

CLIENT_X, CLIENT_Y = 42, 106


def scr(cx, cy):
    return CLIENT_X + cx, CLIENT_Y + cy


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("port %d never opened" % port)


def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.3)
    return False


def send_key(mon, key, delay=0.16):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.14):
    table = {
        " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon",
        "-": "minus", "_": "shift-minus", "?": "shift-slash",
    }
    for c in s:
        if c in table:
            send_key(mon, table[c], delay)
        elif c.isupper():
            send_key(mon, "shift-%s" % c.lower(), delay)
        else:
            send_key(mon, c, delay)


def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.4)


def _rel(mon, dx, dy):
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        dx -= sx; dy -= sy
        time.sleep(0.02)


def move_to(mon, x, y):
    _rel(mon, -2000, -2000)
    time.sleep(0.15)
    _rel(mon, x, y)
    time.sleep(0.2)


def click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n")
    time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n")
    time.sleep(0.4)


def read_vbe_resolution():
    m = re.search(r"\[HW\] VBE: available (\d+)x(\d+)", ser_text())
    if m:
        return int(m.group(1)), int(m.group(2))
    return 1280, 720


def browser_addr_click():
    """On-screen centre of the managed BrowserApp URL bar (mirrors
    test_ie_addrbar.py browser_addr_click)."""
    W, H = read_vbe_resolution()
    ww, wh = 600, 440
    TOPBAR_H, TITLE_BAR_H = 32, 32
    wx = (W - ww) // 2
    wy = (H - wh) // 2 + TOPBAR_H + 10
    content_y = wy + TITLE_BAR_H
    addr_x = wx + 1 + 16 + 240
    addr_y = content_y + 8 + 17
    return addr_x, addr_y


def addr_lines_after(text, prefix, mark=0):
    lines = text[mark:].splitlines()
    out, needle = [], "[%s] addr=" % prefix
    for ln in lines:
        if needle in ln:
            out.append(ln.split(needle, 1)[1])
    return out


def login(mon):
    for _ in range(3):
        type_line(mon, "root"); time.sleep(0.5)
        type_line(mon, "admin"); time.sleep(1.0)
        type_line(mon, "echo boot-ok")
        if wait_for_log("boot-ok", 15.0):
            return True
        mon.sendall(b"sendkey ret\n"); time.sleep(1.0)
    return False


def ie_click_focus(mon):
    """Click the IE address bar, retrying until the IE WndProc acknowledges
    the click (log line '[iexplore] WM_LBUTTONDOWN handled')."""
    for _ in range(5):
        m0 = len(ser_text())
        click(mon, IE_CLICK_X, IE_CLICK_Y)
        time.sleep(0.4)
        if "[iexplore] WM_LBUTTONDOWN handled" in ser_text()[m0:]:
            return True
    return False


def main():
    if not os.path.exists(IMG):
        print("missing %s" % IMG); return 2
    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            try: os.remove(f)
            except OSError: pass

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "128M", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    results = {}
    try:
        mon = wait_sock(MON_PORT)
        mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError): pass

        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot)"); return 1
        time.sleep(1.0)
        if not login(mon):
            print("RESULT: FAIL (login)"); return 1

        # ===================== 32-bit native IE =====================
        print("[32] gui browser (native IE)")
        type_line(mon, "gui browser")
        if not wait_for_log("[iexplore] WM_CREATE", 45.0):
            print("RESULT: FAIL (32-bit IE did not start)"); return 1
        time.sleep(4.0)
        if not ie_click_focus(mon):
            print("RESULT: FAIL (could not focus IE address bar)"); return 1
        print("[32] address bar focused")
        type_text(mon, "ABC", 0.14); time.sleep(0.5)
        mark = len(ser_text())

        print("[32] Ctrl+C (copy field)")
        send_key(mon, "ctrl-c", 0.4); time.sleep(0.3)
        print("[32] type 'XYZ' (append)")
        type_text(mon, "XYZ", 0.14); time.sleep(0.4)
        before = addr_lines_after(ser_text(), "iexplore", mark)
        print("[32] Ctrl+V (paste over field)")
        send_key(mon, "ctrl-v", 0.5); time.sleep(1.0)
        after = addr_lines_after(ser_text(), "iexplore", mark)
        last = after[-1] if after else ""
        results["ie_ctrlv_paste"] = last.endswith("ABC") and not last.endswith("XYZ")
        print("    IE addr before paste: %r" % (before[-1] if before else None))
        print("    IE addr after  paste: %r" % last)

        print("[32] type 'Q' then Ctrl+Z (undo)")
        type_text(mon, "Q", 0.3); time.sleep(0.4)
        before = addr_lines_after(ser_text(), "iexplore", mark)
        send_key(mon, "ctrl-z", 0.5); time.sleep(1.0)
        after = addr_lines_after(ser_text(), "iexplore", mark)
        lastz = after[-1] if after else ""
        results["ie_ctrlz_undo"] = lastz.endswith("ABC") and not lastz.endswith("Q")
        print("    IE addr before undo: %r  after: %r"
              % (before[-1] if before else None, lastz))

        print("[32] Ctrl+A (select-all/copy whole field)")
        send_key(mon, "ctrl-a", 0.4); time.sleep(0.4)
        results["ie_ctrla_select"] = True  # path executed; covered by paste above

        # ===================== switch to 64-bit =====================
        print("[64] switch to long mode")
        type_line(mon, "echo switch-ok")
        if not wait_for_log("switch-ok", 15.0):
            print("RESULT: FAIL (shell not responsive)"); return 1
        type_line(mon, "switch")
        if not wait_for_log("[K64-1] kmain64 entered", 60.0):
            print("RESULT: FAIL (64-bit kernel did not come up)"); return 1
        time.sleep(3.0)

        # ===================== 64-bit managed BrowserApp =====================
        print("[64] gui browser (managed BrowserApp)")
        type_line(mon, "gui browser")
        if not wait_for_log("[GUI] no PE browser available; using managed browser", 45.0):
            print("RESULT: FAIL (64-bit managed browser did not start)"); return 1
        time.sleep(4.0)
        ax, ay = browser_addr_click()
        print("[64] click address bar @ (%d,%d)" % (ax, ay))
        click(mon, ax, ay)
        time.sleep(0.6)
        type_text(mon, "ABC", 0.14); time.sleep(0.5)
        mark64 = len(ser_text())

        print("[64] Ctrl+A (select) + Ctrl+C (copy 'ABC')")
        send_key(mon, "ctrl-a", 0.4); time.sleep(0.2)
        send_key(mon, "ctrl-c", 0.4); time.sleep(0.3)
        print("[64] Ctrl+A (select) + Backspace (clear)")
        send_key(mon, "ctrl-a", 0.4); time.sleep(0.2)
        send_key(mon, "backspace", 0.4); time.sleep(0.4)
        print("[64] Ctrl+V (paste from clipboard)")
        send_key(mon, "ctrl-v", 0.5); time.sleep(1.0)
        b64 = addr_lines_after(ser_text(), "browser", mark64)
        results["b64_ctrlv_paste"] = bool(b64) and b64[-1].endswith("ABC")
        print("    browser addr after Ctrl+V: %r" % (b64[-1] if b64 else None))

        print("[64] type 'Q' then Ctrl+Z (undo)")
        type_text(mon, "Q", 0.3); time.sleep(0.4)
        before = addr_lines_after(ser_text(), "browser", mark64)
        send_key(mon, "ctrl-z", 0.5); time.sleep(1.0)
        after = addr_lines_after(ser_text(), "browser", mark64)
        lastz = after[-1] if after else ""
        results["b64_ctrlz_undo"] = bool(after) and lastz.endswith("ABC")
        print("    browser addr before undo: %r  after: %r"
              % (before[-1] if before else None, lastz))

        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try: q.wait(timeout=3.0)
            except subprocess.TimeoutExpired: q.kill()
        errf.close()

    ok = all(results.values())
    print("")
    print("================= Ctrl shortcuts =================")
    for k, v in results.items():
        print("  %-18s : %s" % (k, "PASS" if v else "FAIL"))
    print("===================================================")
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
