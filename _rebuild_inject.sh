#!/bin/bash
cd /mnt/d/MyOS/bootloader
make build/kernel.bin 2>&1 | grep -iE 'error|undefined|kernel.bin' | head -20
ls -la build/kernel.bin 2>/dev/null && echo "kernel.bin OK" || { echo "NO kernel.bin"; exit 1; }
# inject the freshly built 32-bit kernel into the 32-bit region of os_v2.img (LBA 33)
dd if=build/kernel.bin of=build/os_v2.img bs=512 seek=33 conv=notrunc 2>&1
echo "INJECTED kernel.bin into os_v2.img (LBA 33)"
