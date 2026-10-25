#!/usr/bin/env python3
"""Verify text-input boxes actually receive keystrokes end-to-end.

Launches Notepad via the `gui notepad` command and types a string, then
asserts the Notepad body region of the screen changes (proving the
keystrokes reached NotepadApp.OnKey and were painted).  No debug-log
stubs required -- purely a pixel-diff check.

Screen is 1024x768.  Notepad window is centred: (272,244)..(752,604).
Its text body is painted inside ~(282,292)..(742,594).
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_kb.img"
MPORT = 4491
SPORT = 4492

SER_BUF = bytearray()
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
    sock.settimeout(0.5)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
              ',': 'comma', '=': 'equal'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.12)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.5)


def type_keys(mon, s, gap=0.14):
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else ch
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(gap)


def load_ppm(path):
    for _ in range(40):
        try:
            f = open(path, "rb")
            if f.readline().strip() != b"P6":
                f.close(); time.sleep(0.15); continue
            f.readline(); f.readline()
            d = f.read()
            f.close()
            if len(d) < 720 * 1280 * 3:
                time.sleep(0.15); continue
            return d
        except OSError:
            time.sleep(0.15)
    raise RuntimeError("could not read PPM")


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


def diff_px(d1, d2, x0, y0, x1, y1, tol=12):
    n = 0
    for y in range(y0, y1):
        r1 = y * 1280 * 3
        for x in range(x0, x1):
            i = r1 + x * 3
            a = abs(d1[i] - d2[i]) + abs(d1[i + 1] - d2[i + 1]) + abs(d1[i + 2] - d2[i + 2])
            if a > tol:
                n += 1
    return n


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_kb.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        checks = {}
        # Boot is assumed (image is the same one test-sec boots).  Log in.
        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            time.sleep(1.0)
            break

        # Enter Notepad directly.
        type_line(mon, "gui notepad")
        time.sleep(2.0)
        d_open = grab(mon, "build/kb_np_open.ppm")
        d1 = load_ppm(d_open)

        # Type into the notepad; the body text must change on screen.
        # If the window never opened (active_window stays -1) the keystrokes
        # fall through to the desktop and this region shows no change.
        type_keys(mon, "testinput")
        time.sleep(1.0)
        d_typed = grab(mon, "build/kb_np_typed.ppm")
        d2 = load_ppm(d_typed)
        body_diff = diff_px(d1, d2, 282, 292, 742, 560)
        checks["body_changed"] = body_diff > 150
        print(f"Notepad body pixel diff after typing: {body_diff}")

        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
