#!/usr/bin/env python3
"""Probe QEMU HMP sendkey behaviour and capture monitor responses."""
import os, sys, socket, time, subprocess, struct, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/w32probe.img"
MON_PORT = 4445


def wait_for_socket(port, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


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
            "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
            "-chardev", "file,id=ser,path=build/serial_probe.log",
            "-serial", "chardev:ser",
            "-d", "int",
        ],
        stdout=open("build/qemu_probe_out.log", "wb"),
        stderr=open("build/qemu_probe_err.log", "wb"),
    )

    try:
        mon = wait_for_socket(MON_PORT, 30.0)
        mon.settimeout(2.0)

        def recv():
            try:
                return mon.recv(8192).decode("latin-1", "ignore")
            except socket.timeout:
                return ""

        print("BANNER:", recv())
        time.sleep(4.0)

        mon.sendall(b"info version\n")
        print("info version:", recv())

        mon.sendall(b"sendkey a\n")
        print("sendkey a:", recv())

        mon.sendall(b"sendkey ret\n")
        print("sendkey ret:", recv())

        time.sleep(1.0)
        mon.sendall(b"sendkey l\n")
        mon.sendall(b"sendkey s\n")
        mon.sendall(b"sendkey f\n")
        mon.sendall(b"sendkey s\n")
        mon.sendall(b"sendkey ret\n")
        time.sleep(1.0)
        mon.sendall(b"screendump build/w32_probe.ppm\n")
        time.sleep(1.0)
        print("after screen:", recv())

        mon.sendall(b"quit\n")
        time.sleep(0.5)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    print("\nSerial log:")
    if os.path.exists("build/serial_probe.log"):
        with open("build/serial_probe.log") as f:
            print(f.read())


if __name__ == "__main__":
    main()
