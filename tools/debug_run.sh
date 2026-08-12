#!/usr/bin/env bash
# Debug helper: runs QEMU with serial to stdio so user can interact with UEFI shell
ROOT="/mnt/d/MyOS/bootloader"
cd "$ROOT"

qemu-system-x86_64 \
  -m 128 -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
  -drive file=build/os_uefi.img,format=raw,if=ide \
  -serial stdio -display none

# Usage on Windows PowerShell:
# wsl -d Debian -- bash -lc 'cd /mnt/d/MyOS/bootloader && ./tools/debug_run.sh'
# Then at the UEFI shell prompt type:
# FS0:\EFI\BOOT\BOOTX64.EFI
# and press Enter. Observe kernel serial output in the terminal.
