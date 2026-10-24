#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '=== d_init disassembly ==='
objdump -d -Mintel build_drv.o 2>/dev/null | sed -n '/<d_init>:/,/ret/p' | head -30
echo
echo '=== symbols ==='
nm build_drv.o | head -20
echo
echo '=== relocations on .text ==='
readelf -r elf_test_driver.drv | head -30
