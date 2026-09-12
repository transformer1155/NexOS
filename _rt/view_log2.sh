#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "size: $(stat -c%s build/uefi_boot.log)"
  echo "=== printable content (from line 100) ==="
  tr -c '[:print:]\n' ' ' < build/uefi_boot.log | tr -s ' ' | sed -n '100,260p'
} > build/uefi_boot_view2.txt 2>&1
