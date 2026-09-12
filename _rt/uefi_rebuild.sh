#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== esp.img boo sector FAT type @54 ==="
  dd if=build/esp.img bs=1 skip=54 count=8 2>/dev/null; echo
  echo "=== esp.img ::/EFI/BOOT ==="
  mdir -i build/esp.img '::/EFI/BOOT' 2>&1
  echo "=== esp.img root ==="
  mdir -i build/esp.img '::' 2>&1
  echo "=== rebuild os_uefi.img ==="
  make uefi 2>&1 | tail -14
} > build/uefi_rebuild.txt 2>&1
echo done
