#!/usr/bin/env bash
set -e
cd /mnt/d/MyOS/bootloader || exit 1
bash _rt/build_uefi_wsl.sh
rm -f build/esp.img
dd if=/dev/zero of=build/esp.img bs=1M count=16 2>/dev/null
mformat -i build/esp.img ::
mmd -i build/esp.img ::/EFI
mmd -i build/esp.img ::/EFI/BOOT
mcopy -i build/esp.img build/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i build/esp.img build/kernel.bin ::/kernel.bin
python3 tools/make_gpt_uefi.py build/esp.img build/os_uefi.img build/kernel64.bin build/sfs_uefi.img
echo "REPACK OK $(stat -c%s build/os_uefi.img)"
