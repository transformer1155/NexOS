#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== BOOTX64.EFI build lines ==="
  grep -nE 'BOOTX64.EFI ready|\[4/4\]|IMAGE_BASE|wrote .*BOOTX64|ELF2EFI' build/uefi_make.log | head
  echo "=== artifacts ==="
  ls -l --time-style=+%H:%M build/BOOTX64.EFI build/esp.img build/os_uefi.img
  echo "=== boot log (key markers) ==="
  tr -c '[:print:]\n' ' ' < build/uefi_boot.log | tr -s ' ' | grep -aE 'NexOS UEFI|Loading kernel|SFS staged|GOP:|Graphics OK|kmain entered|RAM-backed image|zfont|Framebuffer init|Entered GUI|lock screen armed|panic|fault|Exception' | head -40
} > build/final_check.txt 2>&1
