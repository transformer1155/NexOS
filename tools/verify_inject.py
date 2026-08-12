#!/usr/bin/env python3
"""Boot a NexOS image and verify a file appears in the SFS listing."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/custom_os.img"
EXPECT = sys.argv[2] if len(sys.argv) > 2 else "myapp.exe"
EXPECT_SNIP = "injected by pack_image.py"
PORT = 4460
LOG = "build/serial_inject.log"

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
    subprocess.run(["cp", IMG, "build/inject_work.img"], check=True)
    for f in (LOG,):
        if os.path.exists(f): os.remove(f)
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/inject_work.img",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        type_line(mon, "vfs myapp.exe")
        time.sleep(2.0)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try: qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)

    data = open(LOG, "rb").read().decode("latin-1", "ignore")
    print("--- serial tail ---")
    print("\n".join(data.splitlines()[-25:]))
    if EXPECT in data and EXPECT_SNIP in data:
        print(f"\nPASS: {EXPECT} is visible and readable inside the VM")
        return 0
    print(f"\nFAIL: {EXPECT} or its content not found in serial log")
    return 1

if __name__ == "__main__":
    sys.exit(main())
