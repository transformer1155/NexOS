#!/usr/bin/env python3
"""Regression: after leaving the GUI (ESC), the text terminal must appear
IMMEDIATELY -- no mouse movement / no cell change should be needed.  This
guards the dirty-cell renderer: on GUI exit it must force a full repaint,
otherwise the LFB keeps showing the desktop until something redraws it.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_exitr.img"
MPORT = 4474
SPORT = 4475

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
            n = 0
            for y in range(160, 561):
                ro = y * 1280 * 3
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
    errf = open("build/qemu_exitr.err", "wb")
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

        # Enter GUI, then ESC out.  Grab the screen WITHOUT any mouse move.
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui)")
            return 1
        time.sleep(2.0)
        mon.sendall(b"sendkey esc\n"); time.sleep(0.6)
        if not wait_for_log("[GUI] Exited GUI mode", 30.0):
            print("RESULT: FAIL (esc)")
            return 1
        time.sleep(0.8)
        grab(mon, "build/exitr_immediate.ppm")
        p = text_pixels("build/exitr_immediate.ppm")
        print(f"terminal text pixels right after ESC (no mouse): {p}")

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        ok = (p > 2000) and not fault
        print("RESULT:", "PASS" if ok else "FAIL",
              {"term_visible": p > 2000, "no_fault": not fault})
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
