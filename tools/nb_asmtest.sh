#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
rm -f /tmp/nca.o
g++ -I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti \
    -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
    -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra -fpermissive \
    -c nexos_compat_asm.cpp -o /tmp/nca.o > /tmp/asm.log 2>&1
echo "compile rc=$?"
cat /tmp/asm.log
if [ -f /tmp/nca.o ]; then
  echo "=== objdump ==="
  objdump -d /tmp/nca.o | sed -n '1,80p'
fi
