#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "size: $(stat -c%s build/uefi_boot.log)"
  echo "=== printable content ==="
  tr -c '[:print:]\n' ' ' < build/uefi_boot.log | tr -s ' ' | head -100
} > build/uefi_boot_view.txt 2>&1
