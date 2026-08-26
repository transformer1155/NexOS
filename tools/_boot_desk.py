#!/usr/bin/env python3
"""Boot os.img, sign in (admin), and screendump the DESKTOP surface.
Usage: python3 tools/_boot_desk.py <out.ppm> [monitor_port]
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
OUT = sys.argv[1] if len(sys.argv) > 1 else "build/desk.ppm"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4477
LOG = "build/_desk_serial.log"
IMG = "build/os.img"


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else ch
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.45)


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    errf = open("build/_desk_qemu.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={IMG}",
        "-m", "4096", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", "file,id=ser,path=" + LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass
        print("booting ... lock screen")
        time.sleep(18.0)
        print("signing in (admin)")
        type_line(mon, "admin")
        time.sleep(3.0)
        mon.sendall(f"screendump {OUT}\n".encode())
        time.sleep(1.5)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()
    if os.path.exists(OUT):
        print("DESKTOP FRAME:", OUT, os.path.getsize(OUT), "bytes")
    else:
        print("FAIL: no desktop frame")


if __name__ == "__main__":
    main()
