#!/usr/bin/env python3
"""Driver for the serial+disk diagnostic UEFI image (os_uefi_diag.img).

Reuses tools/qemu_uefi_capture.py's QEMU harness but points it at the
diagnostic disk + serial log, and forces TCG (WHPX is unavailable on this
host).  The interesting output is build/serial_diag.txt ([K64-IDT],
[DIAG] step=..., [DIAG-FAULT] ...) plus the GOP non-black % captures.
"""
import importlib.util
import os
import sys

PROJ = r"D:\MyOS\bootloader"
spec = importlib.util.spec_from_file_location(
    "qcap", os.path.join(PROJ, "tools", "qemu_uefi_capture.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

m.DISK = os.path.join(m.BUILD, "os_uefi_diag.img")
m.SERIAL = os.path.join(m.BUILD, "serial_diag.txt")

accel = sys.argv[1] if len(sys.argv) > 1 else "tcg"
ncap = int(sys.argv[2]) if len(sys.argv) > 2 else 6
interval = int(sys.argv[3]) if len(sys.argv) > 3 else 10

m.run(accel, ncap, interval)
