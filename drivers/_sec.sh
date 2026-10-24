#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -w -DELF_DEBUG_LAYOUT -I. -o dbg32 _dbg.c driver_loader.c driver_elf.c
./dbg32 ./elf_test_driver.drv 2>&1 | grep -E 'sec|load base|calling init' | head -25
