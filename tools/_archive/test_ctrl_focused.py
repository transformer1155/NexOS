#!/usr/bin/env python3
"""Focused diagnostic: does the 32-bit native IE address bar actually
receive Ctrl+C/V/Z/A as combos (K_CTRL_*) or does QEMU sendkey race and
deliver the bare letter as a WM_CHAR?

Reads the debug markers added to kernel.cpp / gui.cpp / winpe/iexplore.c:
  [kbd] K_CTRL_C/V/Z/A      -> the keyboard driver detected the combo
  [gui] win32 WM_CHAR ch=N  -> a bare letter reached the IE field
  [gui] win32 handle_ctrl code=N -> gui.cpp forwarded WM_NexOS_CTRL
  [iexplore] ctrl code=N clipget=M -> IE received the combo, clip bridge state
  [iexplore] addr=...       -> field content after each edit
"""
import os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os.img"
LOG = "build/serial_focused.log"
MON_ERR = "build/qemu_focused.err"
MON_PORT = 4501
IE_CLICK_X, IE_CLICK_Y = 542, 148
CLIENT_X, CLIENT_Y = 42, 106


def scr(cx, cy): return CLIENT_X + cx, CLIENT_Y + cy


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
    table = {" ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon",
             "-": "minus", "_": "shift-minus", "?": "shift-slash"}
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
        sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        dx -= sx; dy -= sy; time.sleep(0.02)


def move_to(mon, x, y):
    _rel(mon, -2000, -2000); time.sleep(0.15); _rel(mon, x, y); time.sleep(0.2)


def click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.4)


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
    for _ in range(6):
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
    try:
        mon = wait_sock(MON_PORT); mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError): pass
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)"); return 1
        time.sleep(1.0)
        if not login(mon):
            print("RESULT: FAIL (login)"); return 1

        print("[*] gui browser")
        type_line(mon, "gui browser")
        if not wait_for_log("[iexplore] WM_CREATE", 45.0):
            print("RESULT: FAIL (IE no start)"); return 1
        time.sleep(4.0)
        if not ie_click_focus(mon):
            print("RESULT: FAIL (focus)"); return 1
        print("[*] clear address bar")
        for _ in range(40):
            send_key(mon, "backspace", 0.12)
        time.sleep(0.5)
        print("[*] type HELLO")
        type_text(mon, "HELLO", 0.14)
        time.sleep(0.5)
        print("[*] Ctrl+C")
        send_key(mon, "ctrl-c", 0.4); time.sleep(0.3)
        print("[*] type WORLD")
        type_text(mon, "WORLD", 0.14); time.sleep(0.4)
        print("[*] Ctrl+V")
        send_key(mon, "ctrl-v", 0.5); time.sleep(1.0)
        print("[*] type X")
        type_text(mon, "X", 0.3); time.sleep(0.3)
        print("[*] Ctrl+Z")
        send_key(mon, "ctrl-z", 0.5); time.sleep(1.0)
        print("[*] Ctrl+A")
        send_key(mon, "ctrl-a", 0.4); time.sleep(0.4)
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try: q.wait(timeout=3.0)
            except subprocess.TimeoutExpired: q.kill()
        errf.close()

    t = ser_text()
    print("\n===== [kbd] combo detections =====")
    for ln in [l for l in t.splitlines() if l.startswith("[kbd]")]:
        print("  " + ln)
    print("\n===== [gui] win32 path =====")
    for ln in [l for l in t.splitlines() if l.startswith("[gui]")]:
        print("  " + ln)
    print("\n===== [iexplore] ctrl =====")
    for ln in [l for l in t.splitlines() if "[iexplore] ctrl" in l]:
        print("  " + ln)
    print("\n===== [iexplore] addr= (last 20) =====")
    al = [l for l in t.splitlines() if "[iexplore] addr=" in l]
    for ln in al[-20:]:
        print("  " + ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
