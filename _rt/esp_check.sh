#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
OUT=build/esp_check.txt
{
  echo "=== esp.img boot sector (first 96 bytes) ==="
  od -A d -t x1 -N 96 build/esp.img
  echo "=== esp.img FAT-type field @54 ==="
  od -A d -t x1c -j 54 -N 8 build/esp.img
  echo "=== os_uefi.img @LBA16384 (first 96 bytes) ==="
  dd if=build/os_uefi.img bs=512 skip=16384 count=1 2>/dev/null | od -A d -t x1 -N 96
  echo "=== SFS area @LBA4096 (first 16 bytes) ==="
  dd if=build/os_uefi.img bs=512 skip=4096 count=1 2>/dev/null | od -A d -t x1 -N 16
  echo "=== 'BOOTX64 EFI' byte offset inside os_uefi.img ==="
  grep -a -b -o "BOOTX64 EFI" build/os_uefi.img | head -3 | od -A d -t x1c
  echo "=== count of BOOTX64 EFI occurrences ==="
  grep -a -c "BOOTX64 EFI" build/os_uefi.img
  echo "=== count in esp.img ==="
  grep -a -c "BOOTX64 EFI" build/esp.img
} > "$OUT" 2>&1
echo "written $OUT"
