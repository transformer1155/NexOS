#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -m32 -O0 -w -I. -o svmtest _svm_test.c driver_loader.c driver_elf.c 2>&1|head -20
echo build_rc=$?
./svmtest ./svm.drv 2>&1
