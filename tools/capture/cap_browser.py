#!/usr/bin/env python3
"""Capture a screenshot of the NexOS GUI with the Browser window open."""
import os, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# Build the BIOS disk image first; the default 'make' target only builds ISO.
subprocess.run(["make", "build/os.img"], check=True)

IMG = "build/os.img"
WORK = "build/gui_browser.img"
LOG = "build/serial_browser.log"
PORT = 4451

subprocess.run(["cp", IMG, WORK], check=True)
if os.path.exists(LOG):
    os.remove(LOG)
errf = open("build/qemu_browser.err", "wb")
qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-drive", f"format=raw,file={WORK}",
    "-m", "128M",
    "-vga", "std",
    "-display", "none",
    "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    "-chardev", f"file,id=ser,path={LOG}",
    "-serial", "chardev:ser",
], stdout=errf, stderr=errf)


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


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
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        print("booting...")
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(0.5)
        print("entering gui browser...")
        type_line(mon, "gui browser")
        time.sleep(4.0)
        print("screendump browser...")
        mon.sendall(b"screendump build/browser_gui.ppm\n")
        time.sleep(1.5)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    print("\n--- serial tail ---")
    with open(LOG, "rb") as f:
        lines = f.read().decode("latin-1", "ignore").splitlines()
    print("\n".join(lines[-20:]))
    print("\nRESULT:", "PASS" if "[GUI] Entered GUI mode" in open(LOG, "rb").read().decode("latin-1", "ignore") else "FAIL")


if __name__ == "__main__":
    main()
