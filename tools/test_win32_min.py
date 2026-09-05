#!/usr/bin/env python3
"""Minimal headless test: login and run winapp hello32.exe once."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/w32min.img"
PORT = 4446


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
        if 'A' <= ch <= 'Z':
            key = f"shift-{ch.lower()}"
        else:
            key = keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", "file,id=ser,path=build/serial_min.log",
        "-serial", "chardev:ser",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        mon = wait_sock(PORT)
        mon.recv(65536)
        print("booting...")
        time.sleep(8.0)
        print("login root")
        type_line(mon, "root")
        print("password admin")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("running winapp hello32.exe")
        type_line(mon, "winapp hello32.exe")
        print("waiting for GUI...")
        time.sleep(12.0)
        mon.sendall(b"screendump build/w32min_gui.ppm\n")
        time.sleep(1.0)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    with open("build/serial_min.log", "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-30:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
