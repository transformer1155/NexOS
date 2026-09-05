#!/bin/bash
cd /mnt/d/MyOS/bootloader
echo "=== build 32-bit kernel.elf (contains the Agent in distnet.o/net.o) ==="
make build/kernel.elf 2>&1 | grep -iE 'error|undefined|kernel.elf|cannot find|distnet|net\.o' | head -50
echo "EXIT=${PIPESTATUS[0]}"
ls -la build/kernel.elf 2>/dev/null && echo "32-bit kernel.elf BUILT OK" || echo "kernel.elf NOT produced"
