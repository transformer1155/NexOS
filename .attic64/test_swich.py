#!/usr/bin/env python3
"""Reproduce "input swich command hangs" -- verify what `swich` / `switch`
actually do on the 32-bit kernel shell.

Covers:
  1. Boot BIOS os.img -> 32-bit kernel.
  2. Login root/admin.
  3. Type `swich` (a misspelling / non-existent command).
     Expect: "Unknown command: swich" printed, shell still alive
     (we then type `echo MARKER_X` and expect MARKER_X back).
  4. Type `switch` (= switch64).  Expect the 64-bit long-mode kernel to come
     up (banner), i.e. NOT silently hang.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_swich.log"
MON_ERR = "build/qemu_swich.err"
PORT = 4471

K32_MARKERS = ("Shell ready", "[K32]", "SFS:")


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


def send_key(mon, key, delay=0.09):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.07):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.4)


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

        print("[BOOT] waiting for 32-bit (i386) shell...")
        wait_for(K32_MARKERS, 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] type `swich` (misspelled / non-existent command)")
        type_line(mon, "swich")
        time.sleep(1.5)

        print("[SHELL] type `echo MARKER_X` (proves shell alive after swich)")
        type_line(mon, "echo MARKER_X")
        time.sleep(1.5)

        print("[SHELL] type `switch` (= switch64, enter long mode)")
        type_line(mon, "switch")
        # give the 64-bit kernel time to stage + far-jump + boot
        time.sleep(8.0)

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
    print("\n" + "=" * 66)
    print("swich / switch behaviour on the 32-bit shell")
    print("=" * 66)
    checks = {
        "swich_reported_unknown": "Unknown command: swich" in txt,
        "shell_alive_after_swich": "MARKER_X" in txt,
        "switch64_entered_longmode": ("64-bit" in txt) or ("Long mode" in txt)
                                       or ("kernel64" in txt.lower()),
    }
    for k, v in checks.items():
        print("  [%-26s] %s" % (k, "PASS" if v else "FAIL"))
    # show the relevant slice of the log
    idx = txt.find("swich")
    if idx < 0:
        idx = max(0, len(txt) - 400)
    print("\n----- serial slice around the commands -----")
    print(txt[idx-200: idx+600])
    ok = all(checks.values())
    print("\nRESULT:", "PASS (nothing hangs)" if ok else "SEE ABOVE")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
