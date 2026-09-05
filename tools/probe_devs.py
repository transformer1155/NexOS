#!/usr/bin/env python3
"""Probe QEMU devices and sendkey behaviour via TCP HMP monitor."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/w32probe2.img"
PORT = 4446


def wait(port, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("no monitor")


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    qemu = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-drive", f"format=raw,file={WORK}",
            "-m", "128M",
            "-vga", "std",
            "-display", "none",
            "-no-reboot",
            "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        ],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        mon = wait(PORT, 30.0)
        mon.settimeout(2.0)

        def recv():
            try:
                return mon.recv(4096).decode("latin-1", "ignore")
            except socket.timeout:
                return ""

        time.sleep(0.5)
        print("BANNER:", recv().replace("\n", " | "))

        for cmd in ["info version", "info devices", "sendkey a", "sendkey ret", "screendump build/w32_probe2.ppm"]:
            mon.sendall((cmd + "\n").encode())
            time.sleep(0.3)
            print(f"CMD '{cmd}' ->", recv().replace("\n", " | "))

        time.sleep(1.0)
        mon.sendall(b"quit\n")
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)


if __name__ == "__main__":
    main()
