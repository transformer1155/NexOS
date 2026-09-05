#!/bin/bash
cd /mnt/d/MyOS/bootloader
echo "=== build 32-bit kernel.bin ==="
make build/kernel.bin 2>&1 | grep -iE 'error|kernel.bin' | head -20
ls -la build/kernel.bin 2>/dev/null && echo "kernel.bin OK" || { echo "NO kernel.bin"; exit 1; }
echo "=== assemble minimal bootable 32-bit image (boot+stage2+kernel) ==="
if [ ! -f build/boot.bin ] || [ ! -f build/stage2.bin ]; then
  echo "boot/stage2 missing, building..."; make build/boot.bin build/stage2.bin 2>&1 | grep -iE 'error' | head
fi
cat build/boot.bin build/stage2.bin build/kernel.bin > build/os_v2.img
echo "os_v2.img bytes: $(stat -c%s build/os_v2.img)"
echo "DONE"
