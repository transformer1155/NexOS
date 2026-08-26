#!/usr/bin/env python3
"""Boot a *hybrid ISO* the same way a VM would: -cdrom <iso> -boot d,
sign in as root/admin, then screendump the desktop and objectively
verify the portal surface rendered (pixel stats, since the assistant
cannot view images).  Reuse the login flow from test_loginscreen.py.

Usage:  python3 tools/_check_iso.py build/os.iso
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/os.iso"
WORK = "build/_iso_check.img"     # unused, but keeps layout consistent
LOCK = "build/_iso_lock.ppm"
DESK = "build/_iso_desk.ppm"
LOG = "build/_iso_serial.log"
PORT = 4477


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


def read_log():
    try:
        return open(LOG, "rb").read().decode("latin-1", "ignore")
    except Exception:
        return ""


def stats(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    white = accent = 0
    for y in range(20, h - 60):
        for x in range(0, w):
            r, g, b = px[x, y]
            if r > 230 and g > 230 and b > 230:
                white += 1
            if b > 140 and r < 110 and g > 60 and g < 170 and b > r + 40:
                accent += 1
    return w, h, white, accent


def main():
    for f in (LOG, LOCK, DESK):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/_iso_qemu.err", "wb")
    # Boot the ISO as a CD-ROM, exactly like a VM would (VirtualBox/CD boot).
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-cdrom", ISO,
        "-boot", "d",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    ok = True
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass
        print(f"booting ISO {ISO} (cdrom -boot d) ...")
        time.sleep(18.0)

        data = read_log()
        if "[K32] sign-in deferred to the graphical lock screen" in data:
            print("PASS: default graphical boot (lock screen armed)")
        else:
            print("WARN: no 'sign-in deferred' marker"); ok = False
        if "[MFORMS] managed shell ready" in data:
            print("PASS: managed C# shell loaded")
        else:
            print("WARN: managed shell not ready"); ok = False

        mon.sendall(f"screendump {LOCK}\n".encode()); time.sleep(1.5)
        type_line(mon, "admin")
        time.sleep(2.5)
        data = read_log()
        if "[K32-LOGIN] OK user=root" in data:
            print("PASS: root/admin accepted -> desktop")
        else:
            print("FAIL: login not accepted"); ok = False
        mon.sendall(f"screendump {DESK}\n".encode()); time.sleep(1.5)

        if os.path.exists(DESK):
            w, h, wh, ac = stats(DESK)
            print(f"desktop {w}x{h}: near-white={wh} accent-blue={ac}")
            if wh > 40000 and ac > 200:
                print("RESULT: PORTAL-PRESENT (ISO is current)")
            else:
                print("RESULT: NO-PORTAL (ISO is STALE / wrong build)")
                ok = False
        else:
            print("FAIL: no desktop screendump"); ok = False

        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        print("EXCEPTION", ex)
        traceback.print_exc()
        rc = 1
    sys.exit(rc)
