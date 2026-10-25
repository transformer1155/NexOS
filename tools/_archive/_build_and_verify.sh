#!/bin/bash
export PATH=/mingw32/bin:/usr/bin:$PATH
cd /d/MyOS/bootloader
echo "=== which ==="
which gcc g++ ld python3
echo "=== touch sources to force rebuild of kernel + image ==="
touch kernel.cpp linux_compat.cpp usr/libc_impl.c usr/linux_dynlink.c usr/dynlink_crt.c
make 2>&1 | tail -40
echo "MAKE_EXIT=${PIPESTATUS[0]}"
echo "=== timestamps ==="
ls -la build/kernel.bin build/os_textboot.img build/linux_root/gnu_dynlink build/linux_root/libgnu.so 2>&1
