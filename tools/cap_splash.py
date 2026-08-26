#!/usr/bin/env python3
"""Capture the NexOS boot splash (centred Logo + Win11 spinner) and verify it.

Boots build/os.img under QEMU (tcg), watches the serial log for the
[SPLASH] marker, screendumps a handful of frames while the spinner animates,
then waits for the desktop and dumps that too.  Finally it analyses the PPMs
to confirm: deep-blue backdrop, cyan logo present, small gray spinner present.
"""
import os, socket, time, subprocess, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_splash.txt"
PORT = 4461
QEMU = r"D:\qemu\qemu-system-x86_64.exe"

if os.path.exists(LOG):
    os.remove(LOG)

errf = open("build/qemu_splash.err", "wb")
qemu = subprocess.Popen([
    QEMU,
    "-machine", "pc,accel=tcg",
    "-m", "512",
    "-drive", f"format=raw,file={IMG}",
    "-display", "none",
    "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    "-chardev", f"file,id=ser,path={LOG}",
    "-serial", "chardev:ser",
], stdout=errf, stderr=errf)


def wait_sock(port, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def cmd(mon, c):
    mon.sendall((c + "\n").encode())
    time.sleep(0.25)


def main():
    try:
        mon = wait_sock(PORT)
        if qemu.poll() is not None:
            print("[cap] QEMU exited early! stderr:")
            print(open("build/qemu_splash.err", "rb").read().decode("latin-1", "ignore"))
            return 1
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        print("[cap] booting, waiting for [SPLASH] marker...")

        # Wait for the splash animation to begin.
        splash_start = None
        end = time.time() + 90.0
        while time.time() < end:
            if os.path.exists(LOG):
                txt = open(LOG, "rb").read().decode("latin-1", "ignore")
                if "[SPLASH] starting animation" in txt:
                    splash_start = time.time()
                    print("[cap] [SPLASH] marker seen")
                    break
            time.sleep(0.2)
        if splash_start is None:
            print("[cap] ERROR: splash marker never appeared")
            mon.sendall(b"quit\n")
            return 1

        # Capture frames across the spinner animation (~720ms) + a little into load.
        for i in range(10):
            p = f"build/splash_{i:02d}.ppm"
            cmd(mon, f"screendump {p}")
            print(f"[cap] dumped {p}")
            time.sleep(0.12)

        # Wait for desktop (logo should be gone).
        desktop_t = None
        end = time.time() + 60.0
        while time.time() < end:
            txt = open(LOG, "rb").read().decode("latin-1", "ignore")
            if "[GUI] Entered GUI mode" in txt:
                desktop_t = time.time()
                print("[cap] desktop reached")
                break
            time.sleep(0.3)
        time.sleep(1.0)
        cmd(mon, "screendump build/splash_desktop.ppm")
        print("[cap] dumped build/splash_desktop.ppm")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    # ---- analyse ----
    print("\n=== SPLASH FRAME ANALYSIS ===")
    for name in [f"build/splash_{i:02d}.ppm" for i in range(10)] + ["build/splash_desktop.ppm"]:
        if not os.path.exists(name):
            continue
        stats = analyze_ppm(name)
        print(f"{name}: {stats}")

    txt = open(LOG, "rb").read().decode("latin-1", "ignore")
    print("\n--- serial tail ---")
    print("\n".join(txt.splitlines()[-15:]))
    return 0


def analyze_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header
    assert data[:2] == b"P6", path
    idx = 2
    fields = []
    while len(fields) < 3:
        while data[idx] in b" \t\n\r":
            idx += 1
        s = b""
        while data[idx] not in b" \t\n\r":
            s += data[idx:idx+1]; idx += 1
        fields.append(int(s))
    w, h, maxv = fields
    idx += 1  # single whitespace after maxval
    px = data[idx:idx + w*h*3]
    blue = cyan = spin = 0
    n = w * h
    for i in range(0, len(px), 3):
        r = px[i]; g = px[i+1]; b = px[i+2]
        if r < 35 and 5 <= g < 55 and 25 < b < 95:
            blue += 1
        if r < 70 and g > 120 and b > 120:
            cyan += 1
        if abs(r-g) < 22 and abs(g-b) < 22 and r > 120:
            spin += 1
    return (f"{w}x{h} blue={blue*100//n:>3}% cyan={cyan:>5} spin={spin:>4}")


if __name__ == "__main__":
    sys.exit(main())
