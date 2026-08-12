#!/usr/bin/env python3
"""Headless verification of the Chinese-IME feature.

Boots build/os.img (32-bit kernel + C# managed shell) and asserts via
deterministic SERIAL markers (immune to cursor-blink pixel noise):
  [K32] Entering Win11 GUI mode        -> GUI entered
  [GUI] IME mode -> Chinese (...)      -> Shift toggled to ZH
  [GUI] IME mode -> English (EN)       -> Shift toggled back to EN
  [IME] commit U+....                  -> CJK codepoint delivered to C# app
Plus screenshots for visual proof:
  ime_desktop.png / ime_term_*.png / ime_np_*.png
Also checks the top-right clock is gone (visual: language indicator 中/EN).
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_ime.img"
MPORT = 4511
SPORT = 4512
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def type_line(mon, s, gap=0.14):
    for ch in s:
        if 'A' <= ch <= 'Z':
            mon.sendall(f"sendkey shift-{ch.lower()}\n".encode()); time.sleep(gap)
        else:
            key = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
                   '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
                   ',': 'comma', '=': 'equal'}.get(ch, ch)
            mon.sendall(f"sendkey {key}\n".encode()); time.sleep(gap)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.6)


def type_keys(mon, s, gap=0.16):
    for ch in s:
        mon.sendall(f"sendkey {ch}\n".encode()); time.sleep(gap)


def send_key(mon, k, gap=0.18):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def load_ppm(path):
    for _ in range(40):
        try:
            with open(path, "rb") as f:
                assert f.readline().strip() == b"P6"
                wh = f.readline().split()
                w = int(wh[0]); h = int(wh[1])
                f.readline()
                d = f.read()
            if len(d) >= w * h * 3:
                return w, h, d
        except (OSError, AssertionError):
            pass
        time.sleep(0.15)
    raise RuntimeError("could not read PPM " + path)


def grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(f"screendump {path}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared")


def to_png(ppm_path, png_path):
    from PIL import Image
    w, h, d = load_ppm(ppm_path)
    Image.frombytes("RGB", (w, h), d).save(png_path)


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    checks = {}
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # Boot is assumed; login (timing-tolerant).
        time.sleep(8.0)
        type_line(mon, "root"); time.sleep(1.0)
        type_line(mon, "admin"); time.sleep(2.0)
        type_line(mon, "echo boot-ok"); time.sleep(1.5)

        # ---- (1) native terminal IME + Shift toggle on/off/on ----
        type_line(mon, "gui term"); time.sleep(3.0)
        if not ser_contains("[K32] Entering Win11 GUI mode"):
            type_line(mon, "gui term"); time.sleep(3.0)   # retry once
        checks["gui_entered"] = ser_contains("[K32] Entering Win11 GUI mode")
        grab(mon, "build/ime_desktop.ppm")       # topbar (clock area) visible above terminal
        to_png("build/ime_desktop.ppm", "build/ime_desktop.png")

        send_key(mon, "shift"); time.sleep(0.6)   # -> Chinese
        if not ser_contains("[GUI] IME mode -> Chinese"):
            send_key(mon, "shift"); time.sleep(0.6)   # retry toggle
        checks["shift_cn"] = ser_contains("[GUI] IME mode -> Chinese")
        send_key(mon, "shift"); time.sleep(0.6)   # -> English (prove toggle back)
        checks["shift_en"] = ser_contains("[GUI] IME mode -> English")
        send_key(mon, "shift"); time.sleep(0.6)   # -> Chinese (for typing)
        term_idx = len(SER_LINES)   # capture before typing in terminal
        grab(mon, "build/ime_term_open.ppm")
        to_png("build/ime_term_open.ppm", "build/ime_term_open.png")
        type_keys(mon, "ni"); time.sleep(0.8)
        grab(mon, "build/ime_term_compose.ppm")
        to_png("build/ime_term_compose.ppm", "build/ime_term_compose.png")
        send_key(mon, "spc"); time.sleep(0.9)
        grab(mon, "build/ime_term_commit.ppm")
        to_png("build/ime_term_commit.ppm", "build/ime_term_commit.png")
        checks["term_ime_commit_serial"] = any("[IME] commit" in l for l in SER_LINES[term_idx:])

        send_key(mon, "esc"); time.sleep(1.2)     # exit GUI -> shell
        type_line(mon, "echo back"); time.sleep(0.8)

        # ---- (2) managed (C#) Notepad IME ----
        type_line(mon, "gui notepad"); time.sleep(3.0)
        if not ser_contains("[K32] Entering Win11 GUI mode"):
            type_line(mon, "gui notepad"); time.sleep(3.0)
        grab(mon, "build/ime_np_open.ppm")
        to_png("build/ime_np_open.ppm", "build/ime_np_open.png")
        np_idx = len(SER_LINES)
        # Toggle to Chinese (may need a second shift if terminal left IME in CN).
        send_key(mon, "shift"); time.sleep(0.6)
        if not any("[GUI] IME mode -> Chinese" in l for l in SER_LINES[np_idx:]):
            send_key(mon, "shift"); time.sleep(0.6)
        checks["np_shift_cn"] = any("[GUI] IME mode -> Chinese" in l for l in SER_LINES[np_idx:])
        type_keys(mon, "ni"); time.sleep(0.8)
        grab(mon, "build/ime_np_compose.ppm")
        to_png("build/ime_np_compose.ppm", "build/ime_np_compose.png")
        send_key(mon, "spc"); time.sleep(1.0)
        grab(mon, "build/ime_np_commit.ppm")
        to_png("build/ime_np_commit.ppm", "build/ime_np_commit.png")
        checks["np_ime_commit_serial"] = any("[IME] commit" in l for l in SER_LINES[np_idx:])

        ser_dump("build/ime_serial.log")
        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception: pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
