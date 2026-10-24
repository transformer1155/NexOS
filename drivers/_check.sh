#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
CFLAGS="-m32 -ffreestanding -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -Os -Wall -Wextra -I."
echo '=== elf ==='; gcc $CFLAGS -c driver_elf.c -o /tmp/driver_elf.o 2>&1 | head -25; echo "rc=$?"
echo '=== loader ==='; gcc $CFLAGS -c driver_loader.c -o /tmp/driver_loader.o 2>&1 | head -25; echo "rc=$?"
ls -l /tmp/driver_elf.o /tmp/driver_loader.o /tmp/sample_driver.o 2>/dev/null
