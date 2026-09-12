#!/usr/bin/env python3
"""Boot build/os_v2.img headless and screendump at fixed times, to catch the
early boot animation.  Mirrors tools/verify_gui.py's QEMU invocation."""
import os, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG  = "build/os_v2.img"
WORK = "build/splash_work.img"
LOG  = "build/splash_serial.log"
PORT = 18461

SHOTS = [(1, "t01"), (2, "t02"), (3, "t03"), (5, "t05"), (8, "t08"), (40, "t40")]


def find_qemu():
    for c in (r"D:\qemu\qemu-system-x86_64.exe",
              r"C:\Program Files\qemu\qemu-system-x86_64.exe",
              r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"):
        if os.path.exists(c):
            return c
    return "qemu-system-x86_64.exe"


def read_ppm_head(path):
    with open(path, "rb") as f:
        data = f.read(64)
    parts = data.split()
    if len(parts) >= 4 and parts[0] == b"P6":
        return int(parts[1]), int(parts[2])
    return None


def main():
    if not os.path.exists(IMG):
        print("missing " + IMG); return 1
    for f in (WORK, LOG):
        if os.path.exists(f):
            os.remove(f)
    shutil.copy(IMG, WORK)

    qemu = find_qemu()
    args = [qemu, "-machine", "pc", "-drive", "format=raw,file=%s" % WORK,
            "-m", "256M", "-accel", "tcg,tb-size=128", "-vga", "std",
            "-no-reboot",
            "-chardev", "file,id=ser,path=%s" % LOG,
            "-serial", "chardev:ser",
            "-display", "none",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    print("[qemu] " + qemu)
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    mon = None
    end = time.time() + 30
    while time.time() < end:
        try:
            mon = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            mon.settimeout(3.0)
            mon.recv(65536)
            break
        except OSError:
            time.sleep(0.3)
    if mon is None:
        print("monitor not ready"); proc.kill(); return 1

    t0 = time.time()
    for at, tag in SHOTS:
        while time.time() - t0 < at:
            time.sleep(0.1)
        ppm = "build/splash_%s.ppm" % tag
        try:
            if os.path.exists(ppm):
                os.remove(ppm)
        except OSError:
            pass
        mon.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(0.8)
        wh = read_ppm_head(ppm)
        print("  t=%-3ss  %s  %s" % (at, ppm, ("%dx%d" % wh) if wh else "FAILED"))

    try:
        mon.sendall(b"quit\n")
    except OSError:
        pass
    time.sleep(0.5)
    try:
        proc.kill()
    except OSError:
        pass
    print("[done]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
