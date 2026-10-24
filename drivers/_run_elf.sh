#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -O1 -w -I. -o /tmp/elftest _elf_test.c driver_loader.c driver_elf.c 2>&1 | head -30
echo "build_rc=$?"
cp /tmp/elf_test_driver.drv /tmp/test.drv
/tmp/elftest /tmp/test.drv
