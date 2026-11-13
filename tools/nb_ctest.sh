#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
F="-I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -fno-asynchronous-unwind-tables -nostdlib -O2 -fno-strict-aliasing -Wall -Wextra -fpermissive -DMINIOS_HAVE_MFORMS=1 -DNexOS_HAVE_MFORMS=1"
for f in nexos_compat_asm.cpp nexos_compat.cpp osapi.cpp win32.cpp mforms.cpp; do
  echo "=== $f ==="
  g++ $F -c $f -o /tmp/$(basename $f .cpp).o 2>&1 | grep -E "error:" | head -10
done
echo "DONE"
