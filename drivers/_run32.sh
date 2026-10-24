#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '--- rebuild .drv (32-bit) ---'
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Os -w -I. -c _drvtest.c -o /tmp/drv.o
ld -m elf_i386 -r -o /tmp/test.drv /tmp/drv.o
echo '--- build harness as 32-BIT so struct layout matches the driver ---'
gcc -m32 -O0 -w -I. -o /tmp/dbg32 _dbg.c driver_loader.c driver_elf.c
echo "build_rc=$?"
echo '--- run ---'
/tmp/dbg32 /tmp/test.drv 2>&1 | tail -30
