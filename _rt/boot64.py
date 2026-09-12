"""Baseline the 64-bit kernel path.

  1. boot os_v2.img with 0x501E=1  -> 32-bit kernel, text shell, no auto-GUI
  2. wait for the shell banner
  3. type `switch` + Enter through the QEMU monitor (PS/2 keys)
  4. capture the serial log and a screendump

Reports whether the 64-bit long-mode kernel came up, and where it stops.
"""
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_v2.img"
WORK = "build/boot64_work.img"
LOG = "build/boot64_serial.log"
PORT = 18573


def find_qemu():
    for c in (r"D:\qemu\qemu-system-x86_64.exe",
              r"C:\Program Files\qemu\qemu-system-x86_64.exe"):
        if os.path.exists(c):
            return c
    return "qemu-system-x86_64.exe"


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            return True
        time.sleep(0.4)
    return False


def main():
    if not os.path.exists(IMG):
        print("missing " + IMG)
        return 1
    for f in (WORK, LOG):
        if os.path.exists(f):
            os.remove(f)
    shutil.copy(IMG, WORK)

    qemu = find_qemu()
    args = [qemu, "-machine", "pc", "-drive", "format=raw,file=%s" % WORK,
            "-m", "256M", "-accel", "tcg,tb-size=128", "-vga", "std",
            "-no-reboot",
            "-device", "loader,addr=0x501E,data=1,data-len=1",
            "-chardev", "file,id=ser,path=%s" % LOG,
            "-serial", "chardev:ser",
            "-display", "none",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    print("[qemu] boot -> text shell -> type 'switch'")
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
        print("monitor not ready")
        proc.kill()
        return 1

    if not wait_for(("Shell ready", "[K] Command-line shell"), 120):
        print("shell banner not seen; aborting")
        proc.kill()
        return 1
    print("shell up; typing 'switch'")
    time.sleep(2.0)

    for ch in "switch":
        mon.sendall(("sendkey %s\n" % ch).encode())
        time.sleep(0.10)
    mon.sendall(b"sendkey ret\n")
    print("sent; waiting 40 s")

    time.sleep(40)

    log = read_log()
    if "long mode" in log.lower() or "K64" in log or "[64]" in log:
        print("=> 64-bit markers present")
    else:
        print("=> NO 64-bit marker in the log")

    ppm = "build/boot64_screen.ppm"
    try:
        if os.path.exists(ppm):
            os.remove(ppm)
    except OSError:
        pass
    try:
        mon.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(1.0)
        mon.sendall(b"quit\n")
    except OSError:
        pass
    time.sleep(0.5)
    if proc.poll() is None:
        proc.kill()
    print("serial log bytes:", os.path.getsize(LOG) if os.path.exists(LOG) else 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
