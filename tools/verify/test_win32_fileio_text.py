#!/usr/bin/env python3
"""Headless proof (TEXT_BOOT variant) that a REAL 32-bit Windows PE
(hello_fileio.exe, cross-compiled with i686-w64-mingw32-gcc) loads and
executes through the NexOS win32_run loader and exercises genuine Win32
APIs:

  * file READ  -- CreateFileA/ReadFile on an existing SFS file (welcome.txt)
  * file WRITE -- CreateFileA(GENERIC_WRITE|CREATE_ALWAYS)/WriteFile/CloseHandle
                  persisted to the MKFS data FS via kern_fs_create()
  * thread     -- CreateThread() (stage-1 inline execution) links & runs
  * console    -- OutputDebugStringA -> serial markers

After the PE exits we `cat hello.txt` from the shell; cmd_cat() reads the
MKFS data FS, so the echoed body proves the write was really persisted.

Uses build/os_textboot.img (make textboot) which boots to the textual
command line (g_auto_gui=0) so `winapp` is reachable directly.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
LOG = "build/serial_win32_fileio_text.log"
MON_ERR = "build/qemu_win32_fileio_text.err"
PORT = 4481

BOOT_MARKERS = ("Shell ready", "[K1] kmain entered", "PS ")


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
        print("missing %s - run `make textboot` first" % IMG)
        sys.exit(2)

    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            os.remove(f)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
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

        print("[BOOT] waiting for text shell...")
        wait_for(BOOT_MARKERS, 45.0, "shell")
        time.sleep(1.5)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] mkfs   (format + mount the MKFS data disk)")
        type_line(mon, "mkfs")
        time.sleep(1.0)

        print("[SHELL] winapp hello_fileio.exe")
        type_line(mon, "winapp hello_fileio.exe")
        time.sleep(6.0)

        print("[SHELL] cat hello.txt   (verify persistence)")
        type_line(mon, "cat hello.txt")
        time.sleep(2.0)

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
    print("Real Win32 PE (hello_fileio.exe) executed on NexOS (text boot)")
    print("=" * 66)

    checks = {
        "booted":                any(m in txt for m in BOOT_MARKERS),
        "winapp_invoked":        "NexOS Win32 Subsystem" in txt,
        "pe_loaded":             "Image  : hello_fileio.exe" in txt,
        "pe_started":            "[HELLO_PE] start" in txt,
        "file_read_ok":          "read welcome.txt OK" in txt,
        "file_write_persisted":  "wrote hello.txt close=1" in txt,
        "thread_api_ok":         "[HELLO_PE] CreateThread OK" in txt,
        "thread_ran":            "[HELLO_PE] thread worker ran" in txt,
        "pe_done":               "[HELLO_PE] DONE" in txt,
        "write_not_failed":      "write FAILED" not in txt,
        "read_not_failed":       "read FAILED" not in txt,
        "cat_echoes_body":       "Hello from a real Windows PE running on NexOS!" in txt,
        "no_load_failure":       "Load failed" not in txt,
        "no_exception":          "EXCEPTION" not in txt.upper(),
        "no_fault":              ("TRIPLE FAULT" not in txt.upper()) and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- [HELLO_PE] / loader trace ---")
    for line in txt.splitlines():
        if "[HELLO_PE]" in line or "Hello from a real Windows" in line or \
           "NexOS Win32 Subsystem" in line or "Image  :" in line or \
           "Load failed" in line or "Console application finished" in line:
            print("  " + line.rstrip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
