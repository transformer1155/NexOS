#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -w -I. -o dbg2 _dbg2.c driver_loader.c driver_elf.c 2>&1|head
./dbg2 ./elf_test_driver.drv 2>&1|tail -20
