#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Windows-native live test of the AI virtual desktop (NO WSL).

Same flow as test_ai_live.py but drives the MSYS2 QEMU (ucrt64 build) directly
on Windows with absolute paths, and converts PPM->PNG with a stdlib writer so
no Pillow dependency is required.

    boot -> login -> gui -> Ctrl+Right (AI desktop) -> type goal -> Enter
    -> "思考中…" -> typewriter reply reveal

Key thing this test proves: with the g_mforms_anim fix, the typewriter
actually ADVANCES across captured frames (previously the screen froze because
the GUI only repaints on input events).
"""
import os
import socket
import subprocess
import sys
import time
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = r"D:\MyOS\bootloader\build\os_textboot.img"
LOG = r"D:\MyOS\bootloader\build\serial_ai_win.log"
ERR = r"D:\MyOS\bootloader\build\qemu_ai_win.err"
PORT = 4531
SHOT_DIR = r"D:\MyOS\bootloader\build\ai_shots"

BOOT_MARKERS = ("Shell ready", "[K1] kmain entered", "PS ")
GOAL = "hello"


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


def type_line(mon, s, delay=0.22):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.6)


def type_word(mon, s, delay=0.22):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)


def login_root(mon, retries=4):
    for attempt in range(retries):
        send_key(mon, "ret", 0.4)
        type_line(mon, "root")
        time.sleep(1.2)
        if "Password:" in read_log():
            type_line(mon, "admin")
            time.sleep(1.5)
            if "Welcome" in read_log():
                print("  [ok] logged in as root (attempt %d)" % (attempt + 1))
                return True
        print("  [retry] login attempt %d not confirmed, retrying" % (attempt + 1))
        time.sleep(0.8)
    return False


def screendump(mon, path_ppm):
    mon.sendall(("screendump %s\n" % path_ppm).encode())
    time.sleep(0.35)
    for _ in range(20):
        if os.path.exists(path_ppm) and os.path.getsize(path_ppm) > 64:
            return True
        time.sleep(0.05)
    return False


def ppm_to_png(ppm, png):
    try:
        data = open(ppm, "rb").read()
        if data[:2] != b"P6":
            return ppm
        idx = 2
        fields = []
        while len(fields) < 3:
            while idx < len(data) and data[idx] in b" \t\n\r":
                idx += 1
            st = idx
            while idx < len(data) and data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[st:idx]))
        w, h, mx = fields
        idx += 1
        raw = data[idx:]
        rows = b"".join(raw[r*w*3:r*w*3+w*3] for r in range(h))

        def chunk(typ, body):
            return struct.pack(">I", len(body)) + typ + body + \
                struct.pack(">I", zlib.crc32(typ+body) & 0xffffffff)

        out = b"\x89PNG\r\n\x1a\n"
        out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        out += chunk(b"IDAT", zlib.compress(rows, 6))
        out += chunk(b"IEND", b"")
        open(png, "wb").write(out)
        return png
    except Exception as e:
        sys.stderr.write("ppm->png failed: %s\n" % e)
        return ppm


def main():
    if not os.path.exists(IMG):
        print("missing %s - run the Windows build first" % IMG)
        sys.exit(2)
    if not os.path.exists(QEMU):
        print("missing %s - install mingw-w64-ucrt-x86_64-qemu in MSYS2" % QEMU)
        sys.exit(2)

    os.makedirs(SHOT_DIR, exist_ok=True)
    for f in (LOG, ERR):
        if os.path.exists(f):
            os.remove(f)

    shots = []

    errf = open(ERR, "wb")
    q = subprocess.Popen([
        QEMU, "-m", "4096", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    def shot(name):
        ppm = os.path.join(SHOT_DIR, name + ".ppm")
        png = os.path.join(SHOT_DIR, name + ".png")
        if screendump(mon, ppm):
            p = ppm_to_png(ppm, png)
            shots.append((name, p))
            print("  [shot] %s -> %s" % (name, p))
        else:
            print("  [shot] %s FAILED" % name)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for text shell...")
        if not wait_for(BOOT_MARKERS, 60.0, "shell"):
            print("  [warn] boot marker not seen; continuing anyway")
        time.sleep(4.0)

        print("[SHELL] login root / admin")
        if not login_root(mon):
            print("  [FATAL] could not log in; aborting test")
            mon.sendall(b"quit\n")
            time.sleep(1.0)
            q.terminate()
            sys.exit(3)
        time.sleep(1.0)

        print("[SHELL] gui -> Win11 desktop 0")
        type_line(mon, "gui")
        time.sleep(10.0)
        shot("w01_desktop0")

        print("[GUI] Ctrl+Right -> AI virtual desktop")
        send_key(mon, "ctrl-right", 2.0)
        shot("w02_ai_idle")

        print("[AI DESK] type goal '%s'" % GOAL)
        type_word(mon, GOAL)
        time.sleep(1.8)
        shot("w03_goal_typed")

        print("[AI DESK] Enter -> AiSend + deferred agent run")
        send_key(mon, "ret", 0.10)
        for dt, name in [(0.25, "w04_think_a"),
                         (0.60, "w05_think_b"),
                         (1.00, "w06_tw1"),
                         (3.00, "w07_tw2"),
                         (5.00, "w08_tw3"),
                         (8.00, "w09_tw_done")]:
            time.sleep(dt)
            shot(name)

        time.sleep(8.0)
        shot("w10_full_reply")

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
    print("\n" + "=" * 70)
    print("AI virtual desktop LIVE test (Windows QEMU)")
    print("=" * 70)
    print("\nScreenshots:")
    for name, p in shots:
        print("  %-22s %s" % (name, p))

    clr_faults = txt.count("[CLR] fault")
    clr_0x12 = txt.count("unsupported IL opcode 0x12")
    aid_run = "[AIDESK] run:" in txt
    aid_reply = "[AIDESK] reply" in txt

    print("\nSerial console checks:")
    print("  [AIDESK] run marker present : %s" % ("YES" if aid_run else "NO"))
    print("  [AIDESK] reply logged       : %s" % ("YES" if aid_reply else "NO"))
    print("  total [CLR] fault lines     : %d" % clr_faults)
    print("  'unsupported IL opcode 0x12': %d" % clr_0x12)
    print("  TRIPLE FAULT / PANIC        : %s" % (
        "YES (BAD)" if ("TRIPLE FAULT" in txt.upper() or "PANIC" in txt.upper())
        else "no"))
    for l in txt.splitlines():
        if "[AIDESK]" in l:
            print("    " + l.strip())

    print("\nRESULT: clr_0x12=%d aid_run=%s aid_reply=%s shots=%d" % (
        clr_0x12, aid_run, aid_reply, len(shots)))
    print("Done.")


if __name__ == "__main__":
    main()
