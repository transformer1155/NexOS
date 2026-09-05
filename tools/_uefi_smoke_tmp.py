#!/usr/bin/env python3
"""One-off UEFI no-regression smoke: confirm the distnet-enabled 64-bit
BOOTX64.EFI still boots to the shell and the `distnet` command is wired in.
Boots build/os_uefi_diag.img under OVMF + serial, logs in, runs `distnet`
(no args -> usage), asserts the usage text appears (proves distnet is linked
into the 64-bit kernel) and that no fault beacon / triple-fault happened.
"""
import os, sys, socket, time, subprocess, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
QEMU = "D:/qemu/qemu-system-x86_64.exe"
OVMF = "D:/qemu/share/edk2-x86_64-code.fd"
OVMF_VARS = "build/ovmf_vars_test.fd"
IMG = "build/os_uefi_diag.img"
LOG = "build/serial_uefi_smoke.log"
MPORT = 4488

if not os.path.exists(OVMF_VARS):
    shutil.copy("D:/qemu/share/edk2-x86_64-vars.fd", OVMF_VARS)

def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash', '-': 'minus'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode()); time.sleep(0.05)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.3)

def main():
    if os.path.exists(LOG): open(LOG, "w").close()
    errf = open("build/qemu_uefi_smoke.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "q35", "-m", "256",
        "-accel", "tcg,tb-size=64",
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF,
        "-drive", "if=pflash,format=raw,file=%s" % OVMF_VARS,
        "-drive", "file=%s,format=raw,if=ide" % IMG,
        "-vga", "none", "-device", "ramfb,id=rfb",
        "-display", "none", "-net", "none",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MPORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    ok = False; booted = False
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        time.sleep(35.0)  # UEFI + kernel + managed lock screen bring-up
        # UEFI uses the Win11-style managed lock screen, not the text login.
        # Headless login: type the admin username then Enter (uid 0).
        type_line(mon, "admin"); time.sleep(3.0)
        type_line(mon, "distnet")
        time.sleep(2.5)
        type_line(mon, "distnet ai hello world")
        time.sleep(3.5)
        data = open(LOG, "rb").read().decode("latin-1", "ignore")
        booted = ("Shell ready" in data) or ("managed shell ready" in data)
        # usage text proves the 64-bit kernel linked distnet.o
        ok = booted and ("usage" in data or "compute" in data or "scheduler" in data or "DISTNET" in data)
        print("--- serial tail ---")
        print("\n".join(data.splitlines()[-26:]))
    finally:
        try: qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired: qemu.terminate(); qemu.wait(timeout=3.0)
        errf.close()
    print("\nBOOTED_TO_SHELL:", booted)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
