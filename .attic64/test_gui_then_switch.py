#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_gui_then_switch.py - Reproduce the user's scenario:
boot 32-bit -> login -> run `gui` -> exit GUI -> run `switch` ->
check serial for kernel64 life AND screendump the VGA framebuffer.

This tells us whether `switch` is really hung, or just not drawing text
after the GUI left the VGA in a VBE graphics mode.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_gui_switch.log"
MON_ERR = "build/qemu_gui_switch.err"
PORT = 4467
DUMP = "build/gui_switch_screendump.ppm"

K32_MARKERS = ("Shell ready", "SFS:")
K64_MARKERS = ("[K64-1] kmain64 entered", "[K64-5]")


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
            print("    ...%s reached after %.1fs" % (label, timeout - (end - time.time())))
            return True
        time.sleep(0.4)
    print("    !! timed out waiting for %s" % label)
    return False


def send_key(mon, key, delay=0.09):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.07):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.4)


def qemu_args():
    return [
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ]


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        sys.exit(2)

    for f in (LOG, MON_ERR, DUMP):
        if os.path.exists(f):
            os.remove(f)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen(qemu_args(), stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for the 32-bit kernel shell...")
        wait_for(K32_MARKERS, 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] gui   (enter desktop)")
        type_line(mon, "gui")
        # Give the GUI time to paint and the user to 'exit' it.
        time.sleep(6.0)

        # The managed desktop's Terminal shortcut is the '>' key
        # (Desktop.cs: Put(1, Kind.Terminal, ..., '>')).  Activating it
        # calls Shell.ExitGui() which returns to the kernel shell.
        print("[SHELL] sending '>' (Terminal shortcut) to exit GUI -> kernel shell")
        send_key(mon, "shift-period", 0.6)
        time.sleep(2.5)

        print("[SHELL] switch   (enter long mode)")
        type_line(mon, "switch")
        k64_ok = wait_for(K64_MARKERS, 45.0, "kernel64")
        time.sleep(2.0)

        print("[DUMP] capturing VGA framebuffer via screendump")
        mon.sendall(("screendump %s\n" % DUMP).encode())
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
    print("GUI then switch  -  serial + framebuffer report")
    print("=" * 66)

    checks = {
        "32-bit shell reached": any(m in txt for m in K32_MARKERS),
        "64-bit kernel entered": "[K64-1] kmain64 entered" in txt,
        "64-bit hello banner": "[K64-5] Hello world written" in txt,
        "screendump written": os.path.exists(DUMP),
    }
    for name, ok in checks.items():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))

    # Show tail of serial log
    print("\n--- serial tail ---")
    print(txt[-1200:] if txt else "(empty)")

    print("\nFramebuffer dump: %s" % DUMP)
    sys.exit(0 if all(checks.values()) else 1)


if __name__ == "__main__":
    main()
