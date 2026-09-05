#!/usr/bin/env python3
"""End-to-end test for MiniCLR (NexOS .NET milestone 1).

Boots the image, logs in, runs `clr hello.mex`, and verifies that a real
C# assembly -- compiled by Roslyn, flattened by tools/mex_pack.py -- is
interpreted correctly inside the kernel.

Asserted on the serial log:
  * the loader banner  [CLR] loaded hello.mex ...  with unbound_icalls absent
  * managed stdout     CSHARP: hello from managed code
  * arithmetic/loop    CSHARP: sum(1..10)=55        (br/blt/add/stloc path)
  * static call        CSHARP: square(7)=49         (call/ret/mul path)
  * clean exit         CSHARP_OK  and  exited normally
  * no kernel EXCEPTION
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/clr.img"
LOG = "build/serial_clr.log"
PORT = 4455


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
    errf = open("build/qemu_clr.err", "wb")
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
        print("[1] clr hello.mex   (Roslyn-compiled C# on MiniCLR)")
        type_line(mon, "clr hello.mex")
        time.sleep(3.0)
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
        with open("build/qemu_clr.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-30:]))

    checks = [
        ("kernel stayed alive",     "EXCEPTION" not in data),
        ("shell got the command",   "[SHELL] $ clr hello.mex" in data),
        ("image loaded",            "[CLR] loaded hello.mex" in data),
        ("all icalls bound",        "unbound_icalls=" not in data),
        ("managed stdout works",    "CSHARP: hello from managed code" in data),
        ("loop + add correct",      "CSHARP: sum(1..10)=55" in data),
        ("static call + mul ok",    "CSHARP: square(7)=49" in data),
        ("reached end of Main",     "CSHARP_OK" in data),
        ("clean managed exit",      "exited normally" in data),
    ]

    print()
    ok = True
    for name, passed in checks:
        print(f"  [{'ok' if passed else 'XX'}] {name}")
        if not passed:
            ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
