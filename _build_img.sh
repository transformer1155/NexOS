#!/bin/bash
set -e
cd /mnt/d/MyOS/bootloader
python3 _patch_stage2.py
nasm -f bin _stage2.asm -o build/_stage2.bin
echo "stage2_size=$(stat -c %s build/_stage2.bin)"
cat build/boot.bin build/_stage2.bin build/_kernel.bin > build/os_v2_patched.img
echo "img_size=$(stat -c %s build/os_v2_patched.img)"
grep -a -c "serial init done" build/_kernel.bin
echo "DONE"
