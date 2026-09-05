#!/usr/bin/env python3
"""Robustness test: winapp with a missing file must NOT crash the kernel,
and a subsequent winapp of a valid image must still work."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# Default to the text-boot image (auto-GUI OFF) so the shell is reachable
# for the missing-file command, while VBE stays active so `winapp hello32.exe`
# still enters the GUI on demand.  `make test-w32` builds build/os_textboot.img
# and passes it as argv[1].
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
WORK = "build/w32rb.img"
LOG = "build/serial_robust.log"
PORT = 4448


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
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open("build/qemu_robust.err", "wb")
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
            mon.recv(65536)          # drain the QEMU monitor banner
        except (TimeoutError, socket.timeout):
            pass                     # banner may already be consumed
        print("booting...")
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[1] winapp nosuch.exe  (missing file - must not crash)")
        type_line(mon, "winapp nosuch.exe")
        time.sleep(2.0)
        print("[2] winapp hello32.exe  (valid image - must launch GUI)")
        type_line(mon, "winapp hello32.exe")
        print("waiting for GUI...")
        time.sleep(12.0)
        mon.sendall(b"screendump build/w32rb_gui.ppm\n")
        time.sleep(1.5)
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
        print("ERROR: serial log was never created. QEMU stderr:")
        with open("build/qemu_robust.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-30:]))

    ok = True
    if "EXCEPTION" in data:
        print("\nFAIL: kernel exception detected")
        ok = False
    if "[SHELL] $ winapp nosuch.exe" not in data:
        print("\nFAIL: missing-file command never reached the shell")
        ok = False
    if "[GUI] Entered GUI mode" not in data:
        print("\nFAIL: GUI never entered after the valid image")
        ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
