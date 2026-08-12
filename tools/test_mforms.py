#!/usr/bin/env python3
"""End-to-end check for the managed (C#) GUI shell.

Boots os.img, logs in, and runs `gui <app>` so the GUI auto-opens the named
NexOS.Forms application.  Verifies on the serial log that MiniCLR loaded
shell.mex and ran NexOS.Forms.Shell::Init, then captures a screenshot of the
C#-rendered window.

Usage: test_mforms.py [app] [os.img]
  app defaults to "calc".  Try: calc about files tasks control memory term
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

APP = sys.argv[1] if len(sys.argv) > 1 else "calc"
IMG = sys.argv[2] if len(sys.argv) > 2 else "build/os.img"
WORK = "build/mforms_work.img"
LOG = "build/serial_mforms.log"
PPM = "build/mforms_%s.ppm" % APP
PNG = "build/mforms_%s.png" % APP
PORT = 4459


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
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, PPM, PNG):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_mforms.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        print("booting...")
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print(f"[gui {APP}] entering managed GUI, auto-opening {APP}")
        type_line(mon, f"gui {APP}")
        print("waiting for CLR + shell.mex + window render...")
        time.sleep(14.0)
        mon.sendall(f"screendump {PPM}\n".encode())
        time.sleep(2.0)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    if not os.path.exists(LOG):
        print("ERROR: no serial log. QEMU stderr:")
        with open("build/qemu_mforms.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    data = open(LOG, "rb").read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-35:]))

    ok = True
    if "EXCEPTION" in data or "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False
    if "[MFORMS] managed shell ready" in data or "mforms: shell.mex ready" in data:
        print("PASS: MiniCLR loaded shell.mex and ran Shell::Init")
    else:
        print("FAIL: managed shell did not come online")
        ok = False

    # Screenshot -> PNG (best effort; not part of pass/fail)
    if os.path.exists(PPM):
        try:
            subprocess.run([sys.executable, "tools/ppm2png.py", PPM, PNG],
                           check=True)
            print("screenshot:", PNG)
        except Exception as e:
            print("ppm2png failed:", e)
    else:
        print("WARN: no screenshot captured")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
