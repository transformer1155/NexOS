#!/usr/bin/env python3
"""Verify the anti-flicker terminal renderer:
  1. login -> terminal text is visible on the LFB (dirty-cell repaint works)
  2. run a command -> output appears on screen
  3. no fault
Screendumps are analysed for non-black text pixels in the terminal band.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_termr.img"
MPORT = 4472
SPORT = 4473

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
        with _SER_LOCK:
            SER_BUF.extend(data)


def log_text():
    with _SER_LOCK:
        return SER_BUF.decode("utf-8", "replace")


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in log_text():
            return True
        time.sleep(0.25)
    return False


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


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


def text_pixels(path):
    """Count non-black pixels in the terminal band (y 160..560, x 320..960)."""
    for _ in range(40):
        try:
            f = open(path, "rb")
            if f.readline().strip() != b"P6":
                f.close(); time.sleep(0.15); continue
            f.readline(); f.readline()
            d = f.read()
            f.close()
            if len(d) < 1000:
                time.sleep(0.15); continue
            w = 1280
            if len(d) < 720 * w * 3:
                time.sleep(0.15); continue
            n = 0
            for y in range(160, 561):
                ro = y * w * 3
                for x in range(320, 961):
                    i = ro + x * 3
                    if d[i] or d[i + 1] or d[i + 2]:
                        n += 1
            return n
        except OSError:
            time.sleep(0.15)
    raise RuntimeError("could not read PPM")


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_termr.err", "wb")
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
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)")
            return 1
        time.sleep(2.0)
        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        time.sleep(1.0)

        grab(mon, "build/termr_login.ppm")
        p1 = text_pixels("build/termr_login.ppm")
        print(f"login screen text pixels: {p1}")

        type_line(mon, "echo hello-render")
        time.sleep(1.0)
        grab(mon, "build/termr_cmd.ppm")
        p2 = text_pixels("build/termr_cmd.ppm")
        print(f"after command text pixels: {p2}")

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        for line in slog.splitlines():
            if "[FBR]" in line:
                print("kernel:", line.strip())
        ok = (p1 > 2000) and (p2 > p1) and not fault
        print("RESULT:", "PASS" if ok else "FAIL",
              {"text_visible": p1 > 2000, "cmd_updated": p2 > p1, "no_fault": not fault})
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
