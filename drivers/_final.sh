#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -w -I. -o dbg32 _dbg.c driver_loader.c driver_elf.c
echo RUNNING
./dbg32 ./elf_test_driver.drv 2>&1
