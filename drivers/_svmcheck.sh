#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '=== compile svm_driver.c ==='
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Os -Wall -Wextra -I. \
    -c svm_driver.c -o /tmp/svm.o 2>&1 | head -40
echo "rc=${PIPESTATUS[0]}"
ls -l /tmp/svm.o 2>/dev/null
