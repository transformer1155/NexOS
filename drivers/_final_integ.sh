#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== 1. driver_loader.c ==='
gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -Os -Wall -Wextra -Idrivers -c drivers/driver_loader.c -o /tmp/dl.o 2>&1|head -15; echo "rc=$?"
echo '=== 2. driver_elf.c ==='
gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -Os -Wall -Wextra -Idrivers -c drivers/driver_elf.c -o /tmp/de.o 2>&1|head -15; echo "rc=$?"
echo '=== 3. plugin_manager.cpp (with driver boot hook) ==='
g++ -I. -Iplugins -m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -Os -w -c plugins/plugin_manager.cpp -o /tmp/pm.o 2>&1|head -15; echo "rc=$?"
echo '=== 4. svm_driver.c ==='
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Os -Wall -Wextra -Idrivers -c drivers/svm_driver.c -o /tmp/svm.o 2>&1|head -15; echo "rc=$?"
ls -l /tmp/dl.o /tmp/de.o /tmp/pm.o /tmp/svm.o 2>/dev/null
