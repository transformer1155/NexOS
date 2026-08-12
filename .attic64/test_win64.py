#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_win64.py - Headless proof that NexOS's 64-bit long-mode kernel really
*executes* a 64-bit Windows binary (PE32+, machine 0x8664).

Why long mode:  win32.cpp compiles the PE32 executor only for __i386__
(W32_EXEC) and the PE32+ executor only for __x86_64__ (W64_EXEC).  The 32-bit
kernel therefore refuses PE32+ on purpose; the image has to be handed to
kernel64.  We get there the normal user way: boot the BIOS image, log in, and
type `switch64` (kernel.cpp:4643 loads kernel64.bin from LBA 2048 and far-jumps
into long mode).  `--uefi` runs the same flow off build/os_uefi.img instead.

Asserted end to end:
  1. kernel64 is live (long-mode banner on serial)
  2. hello64.exe is found in SFS and parsed as PE32+ (machine 0x8664, x86-64)
  3. the loader routes it to win64_run, not the i386 gate
  4. DIR64 base relocations + the 8-byte IAT are applied
  5. the ms_abi entry point actually runs: the guest calls OutputDebugStringA
     ("[app] Hello from Win64 PE!") then ExitProcess, and control returns
  6. no CPU exception along the way

Usage (must run inside WSL - Windows has no qemu):
    python3 tools/test_win64.py [--uefi]
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

UEFI = "--uefi" in sys.argv
IMG = "build/os_uefi.img" if UEFI else "build/os.img"
LOG = "build/serial_win64.log"
MON_ERR = "build/qemu_win64.err"
VARS = "build/ovmf_vars_win64.fd"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS.fd"
PORT = 4461

# Optional extra case: a real-world native x86-64 binary.  Drop it into
# sfs_files/ (the Makefile packs that directory into the SFS image) and the
# test additionally asserts it is refused for the right reason.
HAVE_DNSPY = os.path.exists("sfs_files/dnspy.exe")

K32_MARKERS = ("Shell ready", "[K32]", "SFS:")
K64_MARKERS = ("[K64-1] kmain64 entered", "[K64-8]", "[K64-5]")


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout, label):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            print("    ...%s reached after %.1fs" % (label, timeout - (end - time.time())))
            return True
        time.sleep(0.4)
    print("    !! timed out waiting for %s" % label)
    return False


def send_key(mon, key, delay=0.09):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.07):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.4)


def qemu_args():
    base = [
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ]
    if UEFI:
        subprocess.run(["cp", OVMF_VARS, VARS], check=True)
        return base + [
            "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF_CODE,
            "-drive", "if=pflash,format=raw,file=%s" % VARS,
            "-drive", "format=raw,file=%s" % IMG,
        ]
    return base + ["-drive", "format=raw,file=%s" % IMG]


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make %s` first" % (IMG, "uefi" if UEFI else "build/os.img"))
        sys.exit(2)

    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            os.remove(f)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen(qemu_args(), stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for the 32-bit kernel shell...")
        wait_for(K32_MARKERS, 45.0, "kernel32 shell")
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[SHELL] switch64   (enter long mode)")
        type_line(mon, "switch64")
        if not wait_for(K64_MARKERS, 45.0, "kernel64"):
            print("    (continuing anyway so the report shows what happened)")
        time.sleep(3.0)

        # 1) inspect only: header dump, no execution
        print("[SHELL] winapp /i hello64.exe")
        type_line(mon, "winapp /i hello64.exe")
        time.sleep(2.5)

        # 2) real execution of the 64-bit guest
        print("[SHELL] winapp hello64.exe")
        type_line(mon, "winapp hello64.exe")
        time.sleep(3.5)

        # 3) a real-world x64 binary must still be refused *accurately*:
        #    dnSpy.exe is a native x86-64 PE32+ launcher (no CLR directory),
        #    so the machine check no longer rejects it - it must be stopped
        #    by the loader's size limit instead, after an honest header dump.
        if HAVE_DNSPY:
            print("[SHELL] winapp dnspy.exe   (expect an accurate refusal)")
            type_line(mon, "winapp dnspy.exe")
            time.sleep(3.5)

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try:
                q.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                q.kill()
    errf.close()

    txt = read_log()

    print("\n" + "=" * 66)
    print("Win64 (PE32+) execution under the 64-bit long-mode kernel  [%s]"
          % ("UEFI" if UEFI else "BIOS+switch64"))
    print("=" * 66)

    checks = {
        "kernel64_live":   any(m in txt for m in K64_MARKERS),
        "file_found":      "file not found" not in txt.lower(),
        "detected_pe32p":  ("0x00008664" in txt or "0x8664" in txt) and "x86-64" in txt,
        "routed_to_win64": "64-bit PE32+ loader" in txt or "[WIN64]" in txt,
        "entry_called":    "[WIN64] calling PE32+ entry" in txt,
        "guest_ran":       "[app] Hello from Win64 PE!" in txt,
        "entry_returned":  "[WIN64] PE32+ entry returned" in txt,
        # the guest returns (actual VA - relocated qword), so rc==0 proves
        # the DIR64 base relocation was applied with the right delta
        "dir64_reloc_ok":  "PE32+ entry returned rc=0000000000000000" in txt,
        "no_exception":    "EXCEPTION" not in txt.upper(),
        "not_rejected":    "requires the 64-bit long-mode kernel" not in txt,
    }
    if HAVE_DNSPY:
        # accurate = it reports the real headers and refuses for the real
        # reason, instead of silently mapping a truncated image
        checks["dnspy_refused"] = "192 KiB limit" in txt
        checks["dnspy_no_crash"] = "[app]" in txt and txt.count("[app]") == 1
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- loader / guest serial trace ---")
    keep = ("winapp", "PE32", "Machine", "x86-64", "ImageBase", "Loaded at",
            "Relocation", "reloc", "Imports", "OutputDebugString", "ExitProcess",
            "[WIN64]", "[app]", "Exit code", "Entry", "Subsystem", "CLR", ".NET",
            "[X]", "Executing", "Sections", "delta")
    for line in txt.splitlines():
        if any(t in line for t in keep):
            print("  " + line.rstrip())

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
