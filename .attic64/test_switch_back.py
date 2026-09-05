#!/usr/bin/env python3
"""Regression: switching 64-bit -> 32-bit (`switch` from the 64-bit kernel)
must NOT hang.

Root cause fixed: cmd_switch32() loaded kernel.bin with KERNEL32_SECTORS=256
(128KB) but the real 32-bit kernel.bin is ~427KB / 827 sectors, so the image
was truncated and the far-jump landed in missing code -> silent hang.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_switchback.log"
MON_ERR = "build/qemu_switchback.err"
PORT = 4472

K32_MARKERS = ("Shell ready", "[K32]", "SFS:")
K64_MARKERS = ("kmain64 entered", "Long mode", "[K64-1]")


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


def wait_after(pos, marker, timeout, label):
    end = time.time() + timeout
    while time.time() < end:
        if marker in read_log()[pos:]:
            return True
        time.sleep(0.4)
    return False


def mon_send_alive(mon):
    """Probe whether the QEMU monitor socket is still connected."""
    try:
        mon.sendall(b"info status\n")
        mon.recv(4096)
        return True
    except (BrokenPipeError, OSError, socket.timeout, TimeoutError):
        return False


def send_key(mon, key, delay=0.09):
    try:
        mon.sendall(("sendkey %s\n" % key).encode())
    except (BrokenPipeError, OSError):
        return False
    time.sleep(delay)
    return True


def type_line(mon, s, delay=0.07):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        if not send_key(mon, key, delay):
            return False
    return send_key(mon, "ret", 0.4)


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        sys.exit(2)
    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            os.remove(f)
    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "128M", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] 32-bit kernel...")
        wait_for(K32_MARKERS, 45.0, "k32")
        time.sleep(2.0)
        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] switch  -> 64-bit long mode")
        type_line(mon, "switch")
        if not wait_for(K64_MARKERS, 30.0, "k64"):
            print("  !! 64-bit kernel did not come up")
        time.sleep(3.0)

        print("[SHELL] switch  -> back to 32-bit (the path that used to hang)")
        pos = len(read_log())
        type_line(mon, "switch")
        # Wait for the 32-bit kernel to come back up (it reboots fresh).
        k32_back = wait_after(pos, "[K1] kmain entered", 30.0, "k32-back")
        if k32_back:
            # Wait until the 32-bit shell is ready for input again.
            wait_after(pos, "Shell ready", 30.0, "k32-shell")
            time.sleep(1.0)
        # If QEMU is still alive, the 32-bit kernel rebooted. The login
        # prompt is VGA-only (not mirrored to serial), so just give it time
        # to reach the shell, then send a benign command sequence. The
        # pass/fail verdict comes from the serial boot markers above, not
        # from the (VGA-only) echo output.
        alive = mon_send_alive(mon)
        print("  [info] QEMU still alive after back-switch: %s" % alive)
        if alive and k32_back:
            time.sleep(12.0)   # let the returned 32-bit kernel reach its shell
            type_line(mon, "root")
            time.sleep(1.0)
            type_line(mon, "admin")
            time.sleep(2.0)
            type_line(mon, "echo MARKER_BACK")
            time.sleep(2.0)

        try:
            mon.sendall(b"quit\n")
        except (BrokenPipeError, OSError):
            pass
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

    # Note: the 32-bit kernel's login prompt and `echo` output are drawn on
    # the VGA/BIOS text console only (not mirrored to the serial port), so we
    # verify the switch-back via the serial markers the 32-bit kernel prints
    # as it boots back up after the far-jump.
    txt = read_log()
    after = txt[pos:]
    checks = {
        "sw64_triggered":     "Switching to 64-bit kernel" in txt,
        "sw32_triggered":     "[K64] Switching to 32-bit kernel" in txt,
        "k32_came_back":      "[K1] kmain entered" in after,
        "k32_shell_ready":    "[K] Command-line shell" in after,
        "k32_fully_booted":   "[K5] Hello world written" in after,
    }
    print("\n" + "=" * 66)
    print("64-bit -> 32-bit switch-back regression")
    print("=" * 66)
    for k, v in checks.items():
        print("  [%-20s] %s" % (k, "PASS" if v else "FAIL"))
    print("\n----- serial slice after the back-switch -----")
    i = after.find("[K64] Switching to 32-bit")
    print(after[max(0, i-40): i+700])
    ok = all(checks.values())
    print("\nRESULT:", "PASS (no hang / 32-bit kernel returns)" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
