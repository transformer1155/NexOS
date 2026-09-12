"""Boot the image with -vga vmware and capture what the SVGA-II driver reports.

Uses the same QEMU options as tools/verify_gui.py except for the VGA device, so
the only variable is the accelerator.  Writes:
    build/svga_serial.log  - serial output (the [SVGA]/[CURSOR] lines live here)
    build/svga_shot.ppm    - one screendump at the desktop
"""
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

MON = 45471
CANDIDATES = [
    r"D:\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]


def find_qemu():
    for c in CANDIDATES:
        if os.path.isabs(c):
            if os.path.exists(c):
                return c
        else:
            return c
    return CANDIDATES[0]


def main():
    img = "build/os_v2.img"
    if not os.path.exists(img):
        print("missing %s - run make first" % img)
        return 1
    work = "build/svga_work.img"
    shutil.copy(img, work)
    for f in ("build/svga_serial.log", "build/svga_shot.ppm", "build/svga.err"):
        if os.path.exists(f):
            os.remove(f)

    args = [find_qemu(), "-machine", "pc", "-drive", "format=raw,file=%s" % work,
            "-m", "256M", "-accel", "tcg,tb-size=128",
            "-vga", "vmware",                      # <-- the only change
            "-no-reboot",
            "-chardev", "file,id=ser,path=build/svga_serial.log",
            "-serial", "chardev:ser",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
            "-display", "none"]
    print("[qemu] " + " ".join(args))
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                         stderr=open("build/svga.err", "wb"))

    mon = None
    end = time.time() + 30
    while time.time() < end:
        try:
            mon = socket.create_connection(("127.0.0.1", MON), timeout=1)
            mon.settimeout(2.0)
            mon.recv(65536)
            break
        except OSError:
            time.sleep(0.3)
    if mon is None:
        print("monitor not ready")
        p.kill()
        return 1

    # Boot to the desktop: TCG plus the boot animation takes a while.
    time.sleep(45)
    for attempt in range(3):
        try:
            mon.sendall(b"screendump build/svga_shot.ppm\n")
        except OSError as e:
            print("monitor write failed: %s" % e)
            break
        # Wait for the file to stop growing rather than guessing at a delay:
        # killing QEMU mid-dump leaves a truncated (or header-only) PPM, which
        # is indistinguishable from "the screendump failed".
        last = -1
        for _ in range(30):
            time.sleep(1.0)
            try:
                sz = os.path.getsize("build/svga_shot.ppm")
            except OSError:
                sz = 0
            if sz > 0 and sz == last:
                break
            last = sz
        try:
            mon.recv(65536)
        except OSError:
            pass
        if last > 1024:
            break
        print("screendump attempt %d produced %d bytes" % (attempt + 1, last))
    try:
        mon.sendall(b"quit\n")
        time.sleep(2)
    except OSError:
        pass
    try:
        p.kill()
    except OSError:
        pass
    print("captured")
    return 0


if __name__ == "__main__":
    sys.exit(main())
