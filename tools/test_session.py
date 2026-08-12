#!/usr/bin/env python3
"""End-to-end: GUI session persistence across a reboot-equivalent flow.

  1. login root/admin
  2. mkfs the data disk (session blobs live there)
  3. `gui calc`        -> calculator opens, GUI runs
  4. ESC               -> gui_exit() auto-saves the session ([SESS] saved 1)
  5. `session`         -> reports 1 saved window
  6. `gui`             -> enter_gui() auto-restores it ([SESS] restored 1)
  7. no fault

Serial is read live over TCP (same pattern as test_anim.py).
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_session.img"
MPORT = 4468
SPORT = 4469
LOG_COPY = "build/serial_session.log"

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
    if os.path.exists(LOG_COPY):
        os.remove(LOG_COPY)
    errf = open("build/qemu_session.err", "wb")
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
        else:
            print("RESULT: FAIL (login)")
            return 1

        # 2. Format the data disk (needed for session blobs).  term.write
        #    output ("MKFS formatted") goes to the screen, not the serial
        #    log, so probe liveness with an echo instead.
        type_line(mon, "mkfs")
        time.sleep(2.0)
        type_line(mon, "echo mkfs-ok")
        mkfs_done = wait_for_log("mkfs-ok", 30.0)
        print(f"mkfs + shell alive     : {mkfs_done}")
        if not mkfs_done:
            print("RESULT: FAIL (mkfs hung the shell)")
            return 1

        # 3. Open the calculator via `gui calc`.
        type_line(mon, "gui calc")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui calc)")
            return 1
        time.sleep(2.0)

        # 4. ESC -> gui_exit() auto-saves the session.
        mon.sendall(b"sendkey esc\n"); time.sleep(0.8)
        saved = wait_for_log("[SESS] saved 1 windows", 30.0)
        print(f"ESC exit saved session : {saved}")

        # 5. `session` reports the saved window (screen output; check the
        #    data via the kernel-side [SESS] save log already captured).
        type_line(mon, "session")
        time.sleep(1.0)
        # 6. `gui` re-enters and restores the calculator.
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui re-enter)")
            return 1
        restored = wait_for_log("[SESS] restored 1 windows", 30.0)
        print(f"`gui` auto-restored    : {restored}")
        time.sleep(1.0)

        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        checks = {
            "no_fault": not fault,
            "mkfs_alive": mkfs_done,
            "saved": saved,
            "restored": restored,
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
