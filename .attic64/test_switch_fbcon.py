#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_switch_fbcon.py - Verify the 64-bit kernel now renders its shell into the
VBE linear framebuffer after `switch` (instead of writing to the invisible
0xB8000 VGA text buffer, which left a black screen under `make play`/GTK).

Asserts:
  1. 64-bit kernel enters (serial marker)
  2. framebuffer console is activated (serial marker)
  3. 64-bit shell prints its banner (serial marker)
  4. a QEMU screendump of the VBE LFB is NOT all-black (text actually painted)
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_switchfbcon.log"
MON_ERR = "build/qemu_switchfbcon.err"
DUMP = "build/switchfbcon.ppm"
PORT = 4472

K64_MARKERS = ("[K64-1] kmain64 entered",
               "[K64-2] framebuffer console active",
               "[K64-5] Hello world written")


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
            print("    ...%s reached" % label)
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
        "qemu-system-x86_64", "-m", "128M", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG, "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ]


def analyze_dump(path):
    """Return (w, h, nonblack_pixels) for a PPM P6 screenshot."""
    with open(path, "rb") as f:
        data = f.read()
    # Parse P6 header: 'P6\n<w> <h>\n<maxval>\n'
    idx = 0
    assert data[0:2] == b"P6", "not a PPM P6 file"
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        start = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        fields.append(int(data[start:idx]))
        idx += 1
    w, h, maxval = fields
    pixels = data[idx:]
    n = 0
    # step through RGB triples
    step = 3
    count = 0
    for i in range(0, len(pixels) - 2, step):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        if r or g or b:
            count += 1
    return w, h, count


def main():
    if not os.path.exists(IMG):
        print("missing %s" % IMG)
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

        print("[BOOT] waiting for 32-bit shell...")
        wait_for(("Shell ready", "SFS:"), 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] switch   (enter long mode)")
        type_line(mon, "switch")
        if not wait_for(K64_MARKERS, 50.0, "kernel64 + fbcon"):
            print("    (continuing to capture framebuffer anyway)")

        time.sleep(2.0)
        print("[DUMP] screendump of the VBE framebuffer")
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
    checks = {
        "64-bit kernel entered": "[K64-1] kmain64 entered" in txt,
        "framebuffer console active": "[K64-2] framebuffer console active" in txt,
        "64-bit banner written": "[K64-5] Hello world written" in txt,
        "screendump written": os.path.exists(DUMP),
    }
    nonblack = 0
    if checks["screendump written"]:
        try:
            w, h, nonblack = analyze_dump(DUMP)
            checks["framebuffer shows text (non-black pixels)"] = nonblack > 200
            print("    framebuffer %dx%d, non-black pixels = %d" % (w, h, nonblack))
        except Exception as e:
            print("    !! dump analysis failed: %s" % e)
            checks["framebuffer shows text (non-black pixels)"] = False

    print("\n" + "=" * 64)
    print("switch -> 64-bit framebuffer console  -  report")
    print("=" * 64)
    for name, ok in checks.items():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    print("\n--- serial tail ---")
    print(txt[-700:])

    ok = all(checks.values())
    print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
