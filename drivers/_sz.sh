#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
echo '--- host 64-bit sizeof ---'
printf '#include "driver_contract.h"\n#include <stdio.h>\nint main(){printf("DriverHeader=%%zu DriverSymbolReq=%%zu ptr=%%zu\\n",sizeof(DriverHeader),sizeof(DriverSymbolReq),sizeof(void*));return 0;}\n' > /tmp/sz.c
gcc -I. /tmp/sz.c -o /tmp/sz && /tmp/sz
echo '--- 32-bit sizeof ---'
gcc -m32 -ffreestanding -nostdlib -I. /tmp/sz.c -o /tmp/sz32 2>/dev/null || gcc -m32 -I. /tmp/sz.c -o /tmp/sz32
/tmp/sz32
echo '--- drv header layout (32-bit) ---'
readelf -x .data /tmp/test.drv | head -8
