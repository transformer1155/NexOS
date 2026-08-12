#!/usr/bin/env python3
"""Headless proof that the Desktop Browser icon launches the real Win64 PE
chrome.exe through the 64-bit long-mode kernel.

Flow asserted:
  1. Boot BIOS os.img, login, switch64 -> kernel64 is live.
  2. Type `gui browser` at the 64-bit shell.
  3. cmd_gui() sets APP_BROWSER as startup app and calls gui_enter().
  4. launch_app(APP_BROWSER) -> launch_chrome_pe() -> launch_pe_exe("chrome.exe")
     -> win32_run() routes to win64_run() -> maps/relocates/executes the PE.
  5. chrome.exe reports its internal markers, creates its window, and paints
     within the 72-entry display-list budget.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_chrome64.log"
MON_ERR = "build/qemu_chrome64.err"
PORT = 4463

K32_MARKERS = ("Shell ready", "[K32]", "SFS:")
K64_MARKERS = ("[K64-1] kmain64 entered", "[K64-8]", "[K64-5]")


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

        print("[BOOT] waiting for 32-bit shell...")
        wait_for(K32_MARKERS, 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] switch64")
        type_line(mon, "switch64")
        if not wait_for(K64_MARKERS, 45.0, "kernel64"):
            print("    (continuing to collect diagnostics)")
        time.sleep(3.0)

        print("[SHELL] gui browser")
        type_line(mon, "gui browser")
        time.sleep(5.0)

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
    print("Chrome PE32+ launched from the 64-bit desktop Browser icon")
    print("=" * 66)

    checks = {
        "kernel64_live":     any(m in txt for m in K64_MARKERS),
        "browser_icon_route":  "[GUI] executing PE image via the win32/win64 loader: chrome.exe" in txt,
        "chrome_started":      "[app] [chrome] MiniPE browser starting" in txt,
        "wm_create":           "[app] [chrome] WM_CREATE" in txt,
        "paint_budget_ok":     "[app] [chrome] paint within display-list budget" in txt,
        "window_surfaced":     "chrome.exe surfaced" in txt,
        "entry_returned":      "[WIN64] PE32+ entry returned" in txt,
        "dir64_reloc_ok":      "PE32+ entry returned rc=0000000000000000" in txt,
        "no_exception":        "EXCEPTION" not in txt.upper(),
        "no_fault":            "TRIPLE FAULT" not in txt.upper() and "PANIC" not in txt.upper(),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- chrome / loader serial trace ---")
    keep = ("[chrome]", "[WIN64]", "PE32+", "win32_run", "chrome.exe",
            "reloc", "Imports", "Executing", "DIR64", "display-list")
    for line in txt.splitlines():
        if any(t in line for t in keep):
            print("  " + line.rstrip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
