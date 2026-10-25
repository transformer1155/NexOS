#!/usr/bin/env python3
"""Full reboot flow for GUI session persistence:

  boot #1: login -> mkfs -> `gui calc` -> ESC (auto-save [SESS] saved 1)
           -> quit QEMU (simulated power-off, disk data persists)
  boot #2: same image -> login -> `gui` -> enter_gui() auto-restores the
           calculator ([SESS] restored 1) -- the "reboot, enter terminal,
           memory comes back" flow.
"""
import os, sys, time, socket, subprocess, threading

IMG = "build/os.img"
WORK = "build/os_session_rb.img"
MPORT = 4470
SPORT = 4471

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


def boot(port_offset, logf):
    """Start QEMU, wait for boot, login, return (qemu, mon, ser)."""
    SER_BUF.clear()
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT + port_offset},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT + port_offset},server,nowait",
    ], stdout=logf, stderr=logf)
    mon = wait_sock(MPORT + port_offset); mon.settimeout(3.0)
    ser = wait_sock(SPORT + port_offset)
    threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
    try: mon.recv(65536)
    except (TimeoutError, socket.timeout): pass
    if not wait_for_log("[K5] Hello world written", 90.0):
        raise RuntimeError("boot failed")
    time.sleep(2.0)
    for attempt in range(3):
        type_line(mon, "root"); time.sleep(0.6)
        type_line(mon, "admin"); time.sleep(1.5)
        type_line(mon, "echo boot-ok")
        if wait_for_log("boot-ok", 20.0):
            return qemu, mon
        mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
    raise RuntimeError("login failed")


def shutdown(mon, qemu):
    try:
        if mon: mon.sendall(b"quit\n")
    except Exception:
        pass
    qemu.terminate()
    try: qemu.wait(timeout=5)
    except Exception: qemu.kill()


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_session_rb.err", "wb")

    # ---- boot #1: mkfs + open calc + ESC (saves session) --------------
    q1, m1 = boot(0, errf)
    try:
        type_line(m1, "mkfs")
        time.sleep(2.0)
        type_line(m1, "echo mkfs-ok")
        if not wait_for_log("mkfs-ok", 30.0):
            print("RESULT: FAIL (mkfs hung)")
            return 1
        type_line(m1, "gui calc")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui calc)")
            return 1
        time.sleep(2.0)
        m1.sendall(b"sendkey esc\n"); time.sleep(0.8)
        saved = wait_for_log("[SESS] saved 1 windows", 30.0)
        print(f"boot#1 ESC saved session: {saved}")
    finally:
        shutdown(m1, q1)
    time.sleep(1.0)

    # ---- boot #2: same disk -> `gui` restores the calculator ----------
    q2, m2 = boot(1, errf)
    try:
        type_line(m2, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (gui on reboot)")
            return 1
        restored = wait_for_log("[SESS] restored 1 windows", 30.0)
        print(f"boot#2 `gui` restored    : {restored}")
        slog = log_text()
        fault = ("TRIPLE FAULT" in slog) or ("EXCEPTION" in slog) or ("PANIC" in slog)
        print("no_fault:", not fault)
        ok = saved and restored and not fault
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        shutdown(m2, q2)


if __name__ == "__main__":
    sys.exit(main())
