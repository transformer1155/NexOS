#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== compile driver framework objects (as Makefile does) ==='
CF='-m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -Os -Wall -Wextra -Idrivers'
gcc $CF -c drivers/driver_loader.c -o /tmp/dl.o 2>&1 | head -20; echo "loader_rc=$?"
gcc $CF -c drivers/driver_elf.c -o /tmp/de.o 2>&1 | head -20; echo "elf_rc=$?"
echo
echo '=== compile plugin_manager.cpp with the new hook ==='
g++ -I. -Iplugins -m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -Os -w -c plugins/plugin_manager.cpp -o /tmp/pm.o 2>&1 | head -20; echo "pm_rc=$?"
ls -l /tmp/dl.o /tmp/de.o /tmp/pm.o 2>/dev/null
