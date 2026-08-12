#!/usr/bin/env python3
"""Test whether keyboard input reaches the Notepad text body."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
WORK = "build/kbnp_work.img"
LOG = "build/serial_kbnp.log"
PPM_B = "build/kbnp_before.ppm"
PPM_A = "build/kbnp_after.ppm"
PORT = 4468


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


def ppm_diff(pa, pb):
    def load(path):
        with open(path, "rb") as f:
            assert f.readline().strip() == b"P6"
            w, h = [int(x) for x in f.readline().split()]
            f.readline()
            data = f.read()
        return w, h, data
    wb, hb, db = load(pb)
    wa, ha, da = load(pa)
    if (wb, hb) != (wa, ha):
        return -1
    n = wb * hb
    diff = 0
    for i in range(n):
        o = i * 3
        if db[o] != da[o] or db[o+1] != da[o+1] or db[o+2] != da[o+2]:
            diff += 1
    return diff


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, PPM_B, PPM_A):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_kbnp.err", "wb")
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
        print("[gui notepad] opening notepad")
        type_line(mon, "gui notepad")
        time.sleep(14.0)
        mon.sendall(f"screendump {PPM_B}\n".encode())
        time.sleep(2.0)
        print("typing 'hello' ...")
        for ch in "hello":
            mon.sendall(f"sendkey {ch}\n".encode()); time.sleep(0.3)
        time.sleep(1.5)
        mon.sendall(f"screendump {PPM_A}\n".encode())
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

    if not os.path.exists(PPM_B) or not os.path.exists(PPM_A):
        print("FAIL: screenshots not captured")
        return 1
    d = ppm_diff(PPM_A, PPM_B)
    print(f"pixel diff before/after typing = {d}")
    if d > 200:
        print("RESULT: PASS (Notepad text changed)")
        return 0
    else:
        print("RESULT: FAIL (Notepad text did NOT change)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
