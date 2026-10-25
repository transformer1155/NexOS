#!/usr/bin/env python3
"""Headless proof that the Desktop Browser icon launches the real Win32
PE32 Internet Explorer (iexplore.exe) through the win32_run loader.

Why this test exists
--------------------
This tree is 32-bit only: the long-mode kernel and its PE32+ browser
(chrome.exe) were retired.  The Browser icon therefore has exactly one
target -- iexplore.exe, an i386 PE32 -- and it must load, execute, create
its window and paint without exhausting the display-list budget.

Flow asserted:
  1. Boot BIOS os.img -> 32-bit kernel.
  2. Login root/admin, then `gui browser` at the shell.
  3. cmd_gui() resolves "browser" to APP_BROWSER and seeds it as the
     startup app; gui_enter() -> launch_app(APP_BROWSER).
  4. launch_browser_pe() -> launch_pe_exe("iexplore.exe"): win32_run
     maps/relocates/executes the i386 PE, which reports its markers,
     creates its window and paints within the display-list budget.
  5. gui_launch_win32() surfaces the window on the managed desktop.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_ie32.log"
MON_ERR = "build/qemu_ie32.err"
PORT = 4464

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

        print("[BOOT] waiting for 32-bit (i386) shell...")
        wait_for(K32_MARKERS, 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        # IMPORTANT: stay on the 32-bit kernel -- do NOT switch64.
        print("[SHELL] gui browser   (32-bit kernel, no switch64)")
        type_line(mon, "gui browser")
        # Managed (C#) Win11 desktop + PE execution can take a while.
        time.sleep(12.0)

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
    print("Internet Explorer (PE32 i386) launched on the 32-bit desktop")
    print("=" * 66)

    checks = {
        "kernel32_live":        any(m in txt for m in K32_MARKERS),
        # The long-mode kernel and its PE32+ browser (chrome.exe) were retired
        # from this tree, so the Browser icon has exactly one target: the
        # i386 PE32 Internet Explorer shell.  Nothing should reference chrome
        # any more -- neither an attempted load nor a fallback message.
        "no_chrome_attempt":    "chrome.exe" not in txt,
        "iexplore_attempted":   "[GUI] executing PE image via the win32/win64 loader: iexplore.exe" in txt,
        "iexplore_started":     "[app] [iexplore] MiniPE browser starting" in txt,
        "wm_create":            "[app] [iexplore] WM_CREATE" in txt,
        "paint_budget_ok":      "[app] [iexplore] paint within display-list budget" in txt,
        "window_created":       ("iexplore.exe surfaced" in txt) or ("Windows created: 1" in txt),
        "no_chrome_on_32bit":   "[app] [chrome]" not in txt,
        "no_exception":         "EXCEPTION" not in txt.upper(),
        "no_fault":             ("TRIPLE FAULT" not in txt.upper()) and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- iexplore / loader serial trace ---")
    keep = ("[iexplore]", "[WIN32]", "win32_run", "iexplore.exe",
            "chrome.exe", "reloc", "Imports", "Machine", "PE32",
            "display-list", "Windows created", "long-mode")
    for line in txt.splitlines():
        if any(t in line for t in keep):
            print("  " + line.rstrip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
