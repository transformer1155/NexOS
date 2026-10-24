#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '--- rebuild .drv ---'
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Os -w -I. -c _drvtest.c -o /tmp/drv.o
ld -m elf_i386 -r -o /tmp/test.drv /tmp/drv.o
ls -l /tmp/test.drv
echo '--- build loader test ---'
gcc -O0 -g -w -I. -o /tmp/dbg _dbg.c driver_loader.c driver_elf.c
echo '--- run ---'
/tmp/dbg /tmp/test.drv 2>&1 | tail -30
