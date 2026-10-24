#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -g -w -I. -o dbg32 _dbg.c driver_loader.c driver_elf.c
which gdb || { echo NO_GDB; exit 0; }
gdb -batch -ex run -ex bt -ex 'info registers eip ebx esp' --args ./dbg32 ./elf_test_driver.drv 2>&1 | tail -30
