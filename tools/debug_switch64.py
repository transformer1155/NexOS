#!/usr/bin/env python3
"""Focused debug: does the 64-bit kernel actually boot after `switch`?

Captures QEMU serial to a FILE (not TCP) so a transition-time connection
drop can't hide the 64-bit kernel's output.  Then inspects the file for
the 64-bit boot markers.
"""
import os, sys, time, socket, subprocess

IMG = "build/os.img"
WORK = "build/os_dbg.img"
MPORT = 4495
SERLOG = "build/serial_dbg.log"


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.07)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.35)


def ser_text():
    try:
        with open(SERLOG, "rb") as f:
            return f.read().decode("latin-1", "replace")
    except OSError:
        return ""


def wait_for(needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.25)
    return False


def main():
    for f in (SERLOG, WORK):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_dbg.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-d", "int", "-D", "build/qemu_int.log",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MPORT,
        "-serial", "file:%s" % SERLOG,
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        print("[BOOT] waiting for 32-bit...")
        if not wait_for("[K5] Hello world written", 90.0):
            print("FAIL boot"); return 1
        time.sleep(2.0)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.2)
            type_line(mon, "echo boot-ok")
            if wait_for("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        print("[GUI] enter")
        type_line(mon, "gui")
        wait_for("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)
        print("[GUI] exit")
        mon.sendall(b"sendkey esc\n"); time.sleep(1.5)
        print("[SWITCH] -> 64-bit")
        type_line(mon, "switch")
        ok = wait_for("[K64-1] kmain64 entered", 35.0)
        print("64-bit kmain64 entered:", ok)
        time.sleep(2.0)
        ok2 = wait_for("[K64] Entering Win11 GUI mode", 25.0)
        print("64-bit GUI marker:", ok2)
        # dump tail
        t = ser_text()
        print("=== serial tail (last 1500 chars) ===")
        print(t[-1500:])
    finally:
        try:
            if mon:
                mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except Exception:
            qemu.kill()
        subprocess.run(["pkill", "-f", "[q]emu-system"], check=False)
    errf.close()


if __name__ == "__main__":
    sys.exit(main())
