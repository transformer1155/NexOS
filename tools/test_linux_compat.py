#!/usr/bin/env python3
"""End-to-end test for the NexOS Linux binary-compat shim (Wine-on-NexOS
Milestone 0): boot, log in, run `linux linuxhello`, and verify a real Linux
ELF executes via the int 0x80 syscall ABI and prints LINUX_OK to the serial
port. Must NOT crash the kernel (no EXCEPTION)."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/linuxcompat.img"
LOG = "build/serial_linuxcompat.log"
PORT = 4451


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
    errf = open("build/qemu_linuxcompat.err", "wb")
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
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[1] linux linuxhello  (Linux ELF via int 0x80 shim)")
        type_line(mon, "linux linuxhello")
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
        print("ERROR: serial log was never created. QEMU stderr:")
        with open("build/qemu_linuxcompat.err", "rb") as f:
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
    if "[SHELL] $ linux linuxhello" not in data:
        print("\nFAIL: `linux linuxhello` never reached the shell")
        ok = False
    if "LINUX_OK" not in data:
        print("\nFAIL: Linux ELF did not print LINUX_OK (syscall shim broken?)")
        ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
