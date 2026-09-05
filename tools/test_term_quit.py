#!/usr/bin/env python3
"""Headless verification: the desktop "Terminal" shortcut drops the GUI and
returns to the text-mode shell, and ESC in the GUI does the same.

Scenarios driven in one QEMU session:

  1. login root/admin
  2. `gui term`   -> launch_app(APP_TERMINAL) intercepts -> gui_exit()
                     assert "[GUI] Exited GUI mode" and the shell answers
                     a follow-up command (shell is alive again)
  3. `gui`        -> enter GUI normally -> "[GUI] Entered GUI mode"
  4. sendkey esc  -> "[GUI] Exited GUI mode" again + shell answers again

Signals (read LIVE over a TCP serial socket, same as test_anim.py):
  * no_fault         -- no TRIPLE FAULT / EXCEPTION / PANIC
  * term_shortcut    -- `gui term` produced an exit
  * shell_alive_1    -- shell answered `echo ok1` after the shortcut exit
  * esc_exit         -- ESC in GUI produced a second exit
  * shell_alive_2    -- shell answered `echo ok2` after the ESC exit
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_term_quit.img"
LOG_COPY = "build/serial_term_quit.log"
MPORT = 4464          # QEMU monitor
SPORT = 4465          # QEMU serial (TCP, live)

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


def _ser_reader(sock, logf):
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
        try:
            logf.write(data); logf.flush()
        except OSError:
            pass


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


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG_COPY,):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_term_quit.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = ser = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        logf = open(LOG_COPY, "wb")
        threading.Thread(target=_ser_reader, args=(ser, logf), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (kernel never finished booting)")
            return 1
        time.sleep(2.0)

        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            print(f"  (login attempt {attempt + 1} did not take, retrying)")
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        else:
            print("RESULT: FAIL (never got a working shell)")
            return 1

        # ---- 1. `gui term` shortcut exits the GUI ------------------------
        n_exit0 = log_text().count("[GUI] Exited GUI mode")
        type_line(mon, "gui term")
        term_shortcut = wait_for_log("[GUI] Exited GUI mode", 30.0)
        time.sleep(1.0)

        # Shell must answer a command after the shortcut exit.
        type_line(mon, "echo ok1")
        shell_alive_1 = wait_for_log("ok1", 20.0)

        # ---- 2. Enter GUI normally, then ESC exits -----------------------
        type_line(mon, "gui")
        gui_entered = wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)
        mon.sendall(b"sendkey esc\n"); time.sleep(0.6)
        esc_exit = wait_for_log("[GUI] Exited GUI mode", 30.0)
        time.sleep(1.0)

        type_line(mon, "echo ok2")
        shell_alive_2 = wait_for_log("ok2", 20.0)

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        n_exit = slog.count("[GUI] Exited GUI mode")

        print(f"  `gui term` produced exit    : {term_shortcut}")
        print(f"  shell answered after exit 1 : {shell_alive_1}")
        print(f"  `gui` entered normally      : {gui_entered}")
        print(f"  ESC produced a 2nd exit     : {esc_exit}")
        print(f"  shell answered after exit 2 : {shell_alive_2}")
        print(f"  total [GUI] Exited log lines: {n_exit} (want >= 2)")

        checks = {
            "no_fault": not fault,
            "term_shortcut": term_shortcut,
            "shell_alive_1": shell_alive_1,
            "gui_entered": gui_entered,
            "esc_exit": esc_exit and n_exit >= 2,
            "shell_alive_2": shell_alive_2,
        }
        ok = all(checks.values())
        print("RESULT:", "PASS" if ok else "FAIL", checks)
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
