#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Headless smoke test for the AI *virtual desktop* chat panel (feature B).

The AI Agent *window* (AiAgent.cs) is covered by test_ai_input.py.  This test
covers the separate AI *virtual desktop* (Desktop.cs CurrentDesktop == 1):
  - Ctrl+Right switches the managed desktop to the AI desktop
    (gui.cpp handle_ctrl code 6 -> mforms_desktop_key(-8) -> Desktop.Key(-8)
     -> SwitchDesktop(1)).
  - On that desktop, printable keys focus + type into the goal box
    (AiDesktopKey), and Enter triggers AiSend() which logs
    "[AIDESK] run: <goal>" and calls Host.Exec("agent run <goal>").

We boot build/os_textboot.img, enter the GUI, switch to the AI desktop, type a
goal + Enter, and assert (a) the [AIDESK] run marker appeared (proves the AI
desktop key/paint/send path executed under MiniCLR without a fault) and
(b) no Triple-Fault / PANIC occurred.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
LOG = "build/serial_ai_desktop.log"
MON_ERR = "build/qemu_ai_desktop.err"
PORT = 4492

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


def send_key(mon, key, delay=0.12):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.18):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.5)


def type_gui(mon, s, delay=0.22):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make textboot` first" % IMG)
        sys.exit(2)

    for f in (LOG, MON_ERR):
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

        print("[SHELL] login root/admin")
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

        print("[SHELL] gui  (enter Win11 GUI desktop 0)")
        type_line(mon, "gui")
        time.sleep(10.0)

        print("[GUI] Ctrl+Right -> switch to AI virtual desktop")
        send_key(mon, "ctrl-right", 1.5)

        print("[AI DESK] typing goal 'hi' into the chat box")
        type_gui(mon, "hi")
        time.sleep(2.0)

        print("[AI DESK] Enter -> AiSend() fires Host.Exec(agent run hi)")
        send_key(mon, "ret", 2.5)

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
    print("AI virtual desktop chat panel smoke (headless, MiniCLR runtime)")
    print("=" * 66)

    checks = {
        "booted":          any(m in txt for m in BOOT_MARKERS),
        "gui_entered":     "[K32] Entering Win11 GUI mode" in txt,
        "aid_run_marker":  "[AIDESK] run:" in txt,   # AiSend() executed
        "no_fault":        ("TRIPLE FAULT" not in txt.upper())
                          and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    # Show the AI desktop send line(s) if present.
    for l in txt.splitlines():
        if "[AIDESK]" in l:
            print("    " + l.strip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
