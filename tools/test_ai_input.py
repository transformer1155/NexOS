#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Headless proof that keyboard input reaches the AI Agent (managed C#) window.

Reproduces the "AI input window can't type" bug: a freshly launched managed
(C#) window was NOT given keyboard focus, so every keystroke was routed to the
desktop and silently lost.  Fix: open_managed_app() now focuses the new
managed window (active_window = wid), and AiAgent opens with editMode = 1 so
its goal box accepts keystrokes immediately.

Uses build/os_textboot.img (make textboot): boots to the textual shell, then
`gui agent` enters the GUI and opens the AI Agent window with focus.  We inject
keystrokes through the QEMU monitor and assert the OS prints
`[gui] managed key ch=<code>` for each typed character -- proof that the
keystroke reached the managed (C#) app (and was not swallowed by the desktop).
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
LOG = "build/serial_ai_input.log"
MON_ERR = "build/qemu_ai_input.err"
PORT = 4491

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
        # refuses single-file deletes when the recycle bin is unavailable.
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

        print("[SHELL] gui agent  (enters GUI + opens AI Agent with focus)")
        type_line(mon, "gui agent")
        time.sleep(12.0)

        print("[GUI] typing 'hello' into the AI goal box")
        type_gui(mon, "hello")
        time.sleep(2.5)

        # Press Enter to run the pipeline (best-effort; proves Enter routes too).
        print("[GUI] pressing Enter (run pipeline, best-effort)")
        send_key(mon, "ret", 1.0)
        time.sleep(1.5)

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
    print("AI Agent (managed C#) keyboard input reaches the window (headless)")
    print("=" * 66)

    mkeys = [l for l in txt.splitlines() if "[gui] managed key ch=" in l]
    print("  managed key dispatch lines seen: %d" % len(mkeys))
    for l in mkeys[:24]:
        print("    " + l.strip())

    checks = {
        "booted":            any(m in txt for m in BOOT_MARKERS),
        "gui_entered":       "[K32] Entering Win11 GUI mode" in txt,
        "managed_focused":   "[gui] managed focus wid=" in txt,
        "mkey_h":            "[gui] managed key ch=104" in txt,   # 'h'
        "mkey_e":            "[gui] managed key ch=101" in txt,   # 'e'
        "mkey_l":            "[gui] managed key ch=108" in txt,   # 'l'
        "mkey_o":            "[gui] managed key ch=111" in txt,   # 'o'
        "no_fault":          ("TRIPLE FAULT" not in txt.upper()) and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    # If the bug were still present, the keystrokes would be routed to the
    # desktop (mforms_desktop_key) and NO managed key line would appear.
    if not mkeys:
        print("  [info] ZERO managed key lines -> bug NOT fixed (keys lost to desktop)")

    # The focus marker proves open_managed_app() now focuses the new window;
    # without it, active_window would stay on the desktop and keys would be
    # lost (exactly the original "AI input window can't type" bug).
    fkeys = [l for l in txt.splitlines() if "[gui] managed focus wid=" in l]
    print("  managed focus lines seen: %d" % len(fkeys))
    for l in fkeys[:8]:
        print("    " + l.strip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
