#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '=== 1. build svm .drv (real ELF32 REL) ==='
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Os -w -I. -c svm_driver.c -o svm.o
ld -m elf_i386 -r -o svm.drv svm.o
ls -l svm.drv
file svm.drv
echo
echo '=== 2. ELF structure ==='
readelf -h svm.drv | head -12
echo '--- sections ---'
readelf -S svm.drv | grep -E '\.text|\.data|\.bss|\.rodata' 
echo '--- key symbols ---'
readelf -s svm.drv | grep -E 'nexos_driver_header|svm_init|svm_unload' 
