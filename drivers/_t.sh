#!/usr/bin/env bash
echo MARK_START
cd /mnt/e/NexOS/NexOS/drivers || { echo CD_FAIL; exit 1; }
ls -l _dbg.c /tmp/test.drv 2>&1 | head
gcc -m32 -O0 -w -I. -o /tmp/dbg32 _dbg.c driver_loader.c driver_elf.c 2>&1 | head -20
echo gcc_done=$?
ls -l /tmp/dbg32 2>&1
/tmp/dbg32 /tmp/test.drv 2>&1 | head -20
echo MARK_END
