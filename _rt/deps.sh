#!/usr/bin/env bash
{
  echo "cygpath: $(command -v cygpath || echo MISSING)"
  echo "wslpath: $(command -v wslpath || echo MISSING)"
  echo "x86_64-elf-gcc:"
  ls -d /mnt/c/Users/trans/elf_tools/bin/x86_64-elf-gcc* 2>&1
  echo "win python:"
  ls -l "/mnt/c/Users/trans/.workbuddy/binaries/python/versions/3.13.12/python.exe" 2>&1
  echo "uefi shim files:"
  ls /mnt/d/MyOS/bootloader/uefi/gf_inc 2>&1
  ls /mnt/d/MyOS/bootloader/uefi/efi_min.c /mnt/d/MyOS/bootloader/uefi/efi_min.lds /mnt/d/MyOS/bootloader/uefi/crt0_min.S 2>&1
} > /mnt/d/MyOS/bootloader/build/deps.txt 2>&1
