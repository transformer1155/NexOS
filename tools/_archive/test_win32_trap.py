#!/usr/bin/env python3
"""Run winapp hello32.exe under QEMU exception tracing to locate the
triple fault that happens inside render_all()."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/w32trap.img"
LOG = "build/serial_trap.log"
QLOG = "build/qemu_int.log"
PORT = 4449


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
    for f in (LOG, QLOG):
        if os.path.exists(f):
            os.remove(f)

    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot", "-no-shutdown",
        "-d", "int,cpu_reset,guest_errors",
        "-D", QLOG,
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        mon = wait_sock(PORT)
        mon.recv(65536)
        print("booting...")
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("winapp hello32.exe")
        type_line(mon, "winapp hello32.exe")
        time.sleep(12.0)
        try:
            mon.sendall(b"quit\n")
        except OSError:
            pass
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    print("\n--- serial tail ---")
    if os.path.exists(LOG):
        with open(LOG, "rb") as f:
            print("\n".join(f.read().decode("latin-1", "ignore").splitlines()[-12:]))

    print("\n--- qemu exception trace (last servicings) ---")
    if os.path.exists(QLOG):
        with open(QLOG, "rb") as f:
            txt = f.read().decode("latin-1", "ignore")
        # Show only the exception-servicing lines, which carry EIP/CR2.
        svc = [l for l in txt.splitlines()
               if l.startswith("     ") and "v=" in l or l.startswith("check_exception")
               or "Triple fault" in l]
        print("\n".join(svc[-25:]) if svc else "(no exception lines)")
        print(f"\n(total qemu log {len(txt)} bytes)")
    else:
        print("(no qemu log)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
