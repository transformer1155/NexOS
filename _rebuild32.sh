#!/bin/bash
cd /mnt/d/MyOS/bootloader
make build/kernel.bin 2>&1 | grep -iE 'error|undefined|kernel.bin' | head -20
ls -la build/kernel.bin 2>/dev/null && echo "kernel.bin OK" || { echo "NO kernel.bin"; exit 1; }
cat build/boot.bin build/stage2.bin build/kernel.bin > build/os_v2.img
echo "rebuilt os_v2.img bytes: $(stat -c%s build/os_v2.img)"
