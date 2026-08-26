#!/usr/bin/env python3
"""Boot build/os_uefi_diag_fixed.img (FIXED kernel64 embedded) in QEMU OVMF
(tcg) and capture serial + GOP non-black%, reusing qemu_uefi_capture.py."""
import importlib.util, os, sys

PROJ = r"D:\MyOS\bootloader"
spec = importlib.util.spec_from_file_location(
    "qcap", os.path.join(PROJ, "tools", "qemu_uefi_capture.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

m.DISK = os.path.join(m.BUILD, "os_uefi_diag_fixed.img")
m.SERIAL = os.path.join(m.BUILD, "serial_diag_fixed.txt")

# ensure a writable OVMF vars file exists
if not os.path.exists(m.OVMF_VARS):
    with open(m.OVMF_VARS, "wb") as f:
        f.write(b"\x00" * (256 * 1024))

accel = sys.argv[1] if len(sys.argv) > 1 else "tcg"
ncap = int(sys.argv[2]) if len(sys.argv) > 2 else 8
interval = int(sys.argv[3]) if len(sys.argv) > 3 else 10
m.run(accel, ncap, interval)
