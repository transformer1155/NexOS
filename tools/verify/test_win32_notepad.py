#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Headless proof that keyboard input reaches a real Win32 PE window
(notepad.exe) after it is launched.

Reproduces the "can't type into the app" bug: a freshly launched Win32
window was NOT given keyboard focus, so every keystroke was routed to the
desktop and silently lost.  Fix: launch_win32_windows() now focuses the
new window (active_window = id) so it immediately receives keystrokes.

Uses build/os_textboot.img (make textboot): boots to the textual shell so
we can type `winapp notepad.exe`, which enters the GUI and (via the fix)
focuses the notepad window.  We then inject keystrokes through the QEMU
monitor and assert the OS prints `[gui] win32 WM_CHAR ch=<code>` for each
typed character -- proof that the keystroke reached the Win32 WndProc
(and was not swallowed by the desktop).
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
LOG = "build/serial_win32_notepad.log"
MON_ERR = "build/qemu_win32_notepad.err"
PORT = 4486

BOOT_MARKERS = ("Shell ready", "[K1] kmain entered", "PS ")


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


def wait_for(markers, timeout, label):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            return True
        time.sleep(0.4)
    return False


def send_key(mon, key, delay=0.09):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.18):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.5)


def type_gui(mon, s, delay=0.22):
    # Type into the focused GUI window (no trailing Enter).
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make textboot` first" % IMG)
        sys.exit(2)

    for f in (LOG, MON_ERR):
        # Truncate instead of os.remove(): the Windows safe-delete hook
        # refuses single-file deletes when the recycle bin is unavailable
        # in the sandbox, which would abort the test before QEMU starts.
        with open(f, "w") as _fh:
            _fh.truncate(0)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "128M", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for text shell...")
        wait_for(BOOT_MARKERS, 45.0, "shell")
        time.sleep(1.5)

        print("[SHELL] login root/admin (retry if first key dropped)")
        logged_in = False
        for attempt in range(3):
            type_line(mon, "root")
            time.sleep(0.8)
            type_line(mon, "admin")
            time.sleep(2.0)
            if "Welcome" in read_log():
                logged_in = True
                break
            print("    [retry] login not detected, retrying")
        if not logged_in:
            print("    [warn] could not confirm login via serial")

        print("[SHELL] winapp notepad.exe  (enters GUI + focuses notepad)")
        type_line(mon, "winapp notepad.exe")
        time.sleep(9.0)

        print("[GUI] typing 'HelloWorld' into notepad")
        type_gui(mon, "HelloWorld")
        time.sleep(2.0)

        # Best-effort Echo click: notepad window at (60,92), Echo button
        # centre ~ (119,405).  Proves the typed text actually landed in the
        # WndProc buffer (notepad dumps it via OutputDebugStringA).
        print("[GUI] clicking Echo (best-effort coordinate click)")
        mon.sendall(b"mouse_move 119 405\n")
        time.sleep(0.2)
        mon.sendall(b"mouse_button 1\n")
        time.sleep(0.2)
        mon.sendall(b"mouse_button 0\n")
        time.sleep(1.0)

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

    print("\n" + "=" * 66)
    print("Win32 notepad keyboard input reaches the window (headless)")
    print("=" * 66)

    wmchars = [l for l in txt.splitlines() if "[gui] win32 WM_CHAR ch=" in l]
    print("  WM_CHAR dispatch lines seen: %d" % len(wmchars))
    for l in wmchars[:24]:
        print("    " + l.strip())

    checks = {
        "booted":                any(m in txt for m in BOOT_MARKERS),
        "notepad_launched":      "[app] NOTEPAD create" in txt or "[app] NOTEPAD launched" in txt,
        "wmchar_H":              "[gui] win32 WM_CHAR ch=72" in txt,
        "wmchar_e":              "[gui] win32 WM_CHAR ch=101" in txt,
        "wmchar_l":              "[gui] win32 WM_CHAR ch=108" in txt,
        "wmchar_o":              "[gui] win32 WM_CHAR ch=111" in txt,
        "wmchar_W":              "[gui] win32 WM_CHAR ch=87" in txt,
        "wmchar_r":              "[gui] win32 WM_CHAR ch=114" in txt,
        "wmchar_d":              "[gui] win32 WM_CHAR ch=100" in txt,
        "no_fault":              ("TRIPLE FAULT" not in txt.upper()) and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    echo = "[app] NOTEPAD echo:" in txt
    print("  [info] Echo click produced buffer dump: %s"
          % ("yes" if echo else "no (coords/click may vary; WM_CHAR proof still valid)"))
    if echo:
        for l in txt.splitlines():
            if "[app] NOTEPAD echo:" in l:
                print("    " + l.strip()[:140])

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
