#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -w -I. -o /tmp/dbg32 _dbg.c driver_loader.c driver_elf.c 2>&1|head
/tmp/dbg32 /tmp/test.drv 2>&1 | tail -15
