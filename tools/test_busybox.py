#!/usr/bin/env python3
"""Run `linux busybox` on the textboot image; capture serial to see how far
the busybox static binary gets before hitting an unsupported syscall."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
WORK = "build/bb.img"
LOG = f"build/serial_bb_{os.getpid()}.log"
# Random high port to dodge TIME_WAIT / leftover-QEMU port collisions.
PORT = 4530 + (os.getpid() % 200)

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

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
              '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
              '=': 'equal', '+': 'shift-equal', '!': 'shift-1'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.06)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)

CMD = sys.argv[1] if len(sys.argv) > 1 else "linux busybox echo hello nexos"

def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG,):
        try: os.remove(f)
        except OSError: pass
    errf = open("build/qemu_bb.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "pc",
        "-drive", f"format=raw,file={WORK},if=ide",
        "-m", "256M", "-accel", "tcg,tb-size=64",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        time.sleep(12.0)
        print("[0] qemu alive:", qemu.poll() is None)
        type_line(mon, "root")
        print("[0.5] after root, qemu alive:", qemu.poll() is None)
        type_line(mon, "admin")
        print("[0.8] after admin, qemu alive:", qemu.poll() is None)
        time.sleep(1.0)
        print("[1] linux busybox")
        type_line(mon, CMD)
        time.sleep(6.0)
        # Grab guest registers to locate the hang point.
        # First drain any pending monitor echo, then issue the command.
        mon.settimeout(0.5)
        try:
            while True:
                if not mon.recv(65536): break
        except (TimeoutError, socket.timeout):
            pass
        mon.sendall(b"info registers\n")
        time.sleep(0.8)
        regs = b""
        mon.settimeout(1.5)
        try:
            while True:
                chunk = mon.recv(65536)
                if not chunk: break
                regs += chunk
                if b"(qemu)" in regs: break
        except (TimeoutError, socket.timeout):
            pass
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        txt = regs.decode("latin-1", "ignore")
        # Keep only the register lines (EIP=... EAX=... etc).
        import re
        m = re.search(r"EIP=\S+.*?CR0=[0-9a-fA-F]+", txt, re.S)
        print("[regs]", m.group(0)[:1500] if m else txt[-1500:])
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try: qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-45:]))
    # summary of unsupported syscalls
    import re
    uns = re.findall(r"linux: unsupported syscall (\d+)", data)
    if uns:
        from collections import Counter
        print("\nunsupported syscalls:", dict(Counter(uns)))

if __name__ == "__main__":
    main()
