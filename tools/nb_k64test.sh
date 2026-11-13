#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
SFS_LBA=$(grep -c . /dev/null)  # placeholder
g++ -I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -fno-asynchronous-unwind-tables -nostdlib -O2 -fno-strict-aliasing -Wall -Wextra -fpermissive -DMINIOS_HAVE_MFORMS=1 -DNexOS_HAVE_MFORMS=1 -Idrivers -DSFS_LBA_FROM_MAKEFILE=6144 -c .attic64/kernel64.cpp -o /tmp/k64.o 2>&1 | grep -E "error:" | head -25
echo "=== k64 rc check done ==="
