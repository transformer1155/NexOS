#!/usr/bin/env python3
"""Diagnostic: take QEMU screendumps at several boot moments and report
whether the captured frames differ at all.  Used to validate that the
headless screenshot path actually reflects guest video output."""
import os, sys, socket, time, subprocess
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/probe.img"
PORT = 4455


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def summarize(path):
    if not os.path.exists(path):
        return "missing"
    with open(path, "rb") as f:
        d = f.read()

    def tok(i):
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        return d[i:j], j

    _, i = tok(0)
    w, i = tok(i)
    h, i = tok(i)
    _, i = tok(i)
    i += 1
    w, h = int(w), int(h)
    px = d[i:]
    c = Counter()
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            o = (y * w + x) * 3
            c[px[o:o + 3]] += 1
    top = ", ".join(f"{bytes(k).hex()}x{v}" for k, v in c.most_common(4))
    return f"{w}x{h} colors={len(c)} top=[{top}]"


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
        "-chardev", "file,id=ser,path=build/probe_serial.log",
        "-serial", "chardev:ser",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    shots = []
    try:
        mon = wait_sock(PORT)
        time.sleep(0.3)
        mon.recv(65536)
        for t in (1.0, 2.0, 4.0, 8.0, 14.0):
            time.sleep(t if not shots else t - shots[-1][0])
            name = f"build/probe_{t:g}.ppm"
            mon.sendall(f"screendump {name}\n".encode())
            time.sleep(0.6)
            shots.append((t, name))
        mon.sendall(b"info vga\n")
        time.sleep(0.4)
        try:
            mon.settimeout(1.0)
            print("--- info vga ---")
            print(mon.recv(65536).decode("latin-1", "ignore"))
        except Exception:
            pass
        mon.sendall(b"quit\n")
        time.sleep(0.4)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    print("--- frames ---")
    prev = None
    for t, name in shots:
        info = summarize(name)
        same = ""
        if prev and os.path.exists(name) and os.path.exists(prev):
            same = " SAME-AS-PREV" if open(name, "rb").read() == open(prev, "rb").read() else " (changed)"
        print(f"t={t:>4}s  {info}{same}")
        prev = name
    return 0


if __name__ == "__main__":
    sys.exit(main())
