#!/usr/bin/env python3
"""Headless reproduction of the `gui` command triple-fault.

Boots os.img, logs in (root/admin), types `gui`, and captures the serial
log.  Because the desktop paint triple-faults, QEMU (with -no-reboot) exits;
the serial chardev file still contains every byte the guest emitted before
the reset, so the last `[icall] ...` / `[clr] ...` line pins the culprit.
"""
import os, sys, time, subprocess, socket, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/guicrash.img"
LOG = "build/serial_guicrash.log"
MONPORT = 4461

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"


def type_line(mon, s):
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else ch
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.07)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def main():
    if not os.path.exists(IMG):
        print("ERROR: %s not found" % IMG)
        return 1
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG,):
        if os.path.exists(f):
            os.remove(f)

    errf = open("build/qemu_guicrash.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "pc",
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONPORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        # wait for monitor
        end = time.time() + 30
        mon = None
        while time.time() < end:
            try:
                mon = socket.create_connection(("127.0.0.1", MONPORT), timeout=0.5)
                break
            except OSError:
                time.sleep(0.2)
        if mon is None:
            print("FAIL: QEMU monitor not ready")
            return 1
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except Exception:
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)
        print("[harness] typing 'gui'")
        type_line(mon, "gui")
        # Let it render several frames / animation ticks.
        time.sleep(6.0)
        try:
            mon.sendall(b"screendump build/gui_shot.ppm\n")
        except Exception:
            pass
        time.sleep(6.0)
        # Probe liveness: a sendkey should succeed if QEMU is still up.
        alive = True
        try:
            mon.sendall(b"sendkey spc\n")
        except Exception:
            alive = False
        print("[harness] QEMU still alive after gui: %s" % alive)
        try:
            mon.sendall(b"quit\n")
        except Exception:
            pass
    finally:
        try:
            qemu.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        errf.close()

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    n_paint = sum(1 for l in lines if l.strip() == "[clr] NexOS.Forms.Shell::PaintDesktop")
    n_begin = sum(1 for l in lines if "render_all begin" in l)
    n_exc   = sum(1 for l in lines if "EXCEPTION" in l or "triple" in l.lower())
    print("\nframes painted (PaintDesktop calls): %d" % n_paint)
    print("render_all begin markers: %d" % n_begin)
    print("EXCEPTION/triple mentions: %d" % n_exc)
    print("\n--- last 12 serial lines ---")
    print("\n".join(lines[-12:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
