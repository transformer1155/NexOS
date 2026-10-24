#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== make 编译 driver framework + link 验证（只看这两步）==='
rm -f build/driver_loader.o build/driver_elf.o
make build/driver_loader.o build/driver_elf.o 2>&1 | tail -12
echo "make_rc=$?"
ls -l build/driver_loader.o build/driver_elf.o 2>/dev/null
