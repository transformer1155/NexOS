#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
# 1) build the driver as a real ELF32 relocatable (.drv)
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Os -Wall -I. \
    -c _drvtest.c -o /tmp/elf_test_driver.o 2>&1 | head -20
echo "drv_obj_rc=$?"
# 2) link to a relocatable ELF image with sections (ET_REL kept via -r)
ld -m elf_i386 -r -o /tmp/elf_test_driver.drv /tmp/elf_test_driver.o 2>&1 | head -20
echo "drv_link_rc=$?"
file /tmp/elf_test_driver.drv 2>/dev/null || true
readelf -h /tmp/elf_test_driver.drv 2>/dev/null | head -20
echo '--- sections ---'
readelf -S /tmp/elf_test_driver.drv 2>/dev/null | head -30
echo '--- symbols of interest ---'
readelf -s /tmp/elf_test_driver.drv 2>/dev/null | grep -i 'nexos_driver_header\|kmalloc\|printk' | head
