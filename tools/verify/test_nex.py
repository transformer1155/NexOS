#!/usr/bin/env python3
"""Headless verification for P1 basic runtime:  run hello.nex natively.

Boots the text-boot BIOS image (auto-GUI OFF so the textual shell is
reachable), logs in (root / admin), types `run hello.nex`, and asserts
that the native NexOS user runtime actually executed:

  * "Running NexOS native app: hello.nex"  (cmd_run routed .nex -> linux_run)
  * "Hello NexOS"                          (printf -> int 0x80 sys_write)
  * "NEX: Hello from the heap"             (malloc + strcpy + printf)
  * "[NEX] ALL CHECKS PASSED"              (libc + heap smoke test)

The guest writes to COM1 via the int 0x80 syscall ABI, which QEMU's
`-serial file:` captures into the log we assert on.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
SER = "build/serial_nex.log"
PORT = 4483


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
        time.sleep(0.06)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def main():
    if not os.path.exists(IMG):
        print(f"ERROR: image not found: {IMG}  (run `make textboot`)")
        return 1
    subprocess.run(["cp", IMG, "build/nex.img"], check=True)
    for f in (SER,):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_nex.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/nex.img",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={SER}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    ok = False
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

        print("[1] run hello.nex")
        type_line(mon, "run hello.nex")
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

    if not os.path.exists(SER):
        print("ERROR: serial log missing. QEMU stderr:")
        with open("build/qemu_nex.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(SER, "rb") as f:
        data = f.read().decode("latin-1", "ignore")

    print("\n--- serial log (hello.nex region) ---")
    for ln in data.splitlines():
        if "NexOS" in ln or "NEX" in ln or "hello" in ln or "Running" in ln:
            print("   ", ln.strip())

    def need(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False
        else:
            print("PASS:", msg)

    ok = True
    need("Running NexOS native app: hello.nex" in data,
         "cmd_run routed .nex -> linux_run")
    need("Hello NexOS" in data, "guest printf reached COM1 (int 0x80 sys_write)")
    need("NEX: Hello from the heap" in data, "heap: malloc/strcpy/printf worked")
    need("NEX: calloc zeroed? 0000" in data, "calloc zeroed memory")
    need("NEX: realloc -> tiny-grown" in data, "realloc grew the buffer")
    need("[NEX] ALL CHECKS PASSED" in data, "full libc + heap smoke test passed")
    need("EXCEPTION" not in data, "no kernel exception during guest run")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
