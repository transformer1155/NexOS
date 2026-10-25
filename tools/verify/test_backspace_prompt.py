#!/usr/bin/env python3
"""Regression: backspace must never delete the shell prompt
("PS user@NexOS /path> ").  Type some text, delete it all, then press
backspace several more times -- the prompt text must survive and the
shell must still accept commands.

Checked by comparing text-pixel counts: with the bug, extra backspaces
eat the prompt and the count drops well below the clean-prompt baseline.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_bs.img"
MPORT = 4476
SPORT = 4477

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


def send_keys(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)


def type_line(mon, s):
    send_keys(mon, s)
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
    errf = open("build/qemu_bs.err", "wb")
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

        # Baseline: clean prompt, no input yet.
        grab(mon, "build/bs_base.ppm")
        p_base = text_pixels("build/bs_base.ppm")
        print(f"clean prompt text pixels : {p_base}")

        # Type "abc" (no Enter), then delete it and press backspace 8 more
        # times -- far beyond the 3 typed chars.
        send_keys(mon, "abc")
        time.sleep(0.5)
        grab(mon, "build/bs_typed.ppm")
        p_typed = text_pixels("build/bs_typed.ppm")
        print(f"with 'abc' typed        : {p_typed}")

        for _ in range(11):
            mon.sendall(b"sendkey backspace\n")
            time.sleep(0.12)
        time.sleep(0.8)
        grab(mon, "build/bs_deleted.ppm")
        p_deleted = text_pixels("build/bs_deleted.ppm")
        print(f"after delete + 8 extra  : {p_deleted}")

        # Prompt survives if we end near the baseline (>= 90% of it).
        survived = p_deleted >= int(p_base * 0.9)
        print(f"prompt survived         : {survived}")

        # Shell must still work.
        type_line(mon, "echo still-alive")
        alive = wait_for_log("still-alive", 20.0)
        print(f"shell still accepts cmd : {alive}")

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        ok = survived and alive and not fault
        print("RESULT:", "PASS" if ok else "FAIL",
              {"prompt_survived": survived, "shell_alive": alive, "no_fault": not fault})
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
