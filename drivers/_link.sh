#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== 符号检查 ==='
nm build/driver_loader.o | grep -E ' T | D ' | head -12
echo '--- 未定义符号（应由 kernel 提供）---'
nm -u build/driver_loader.o | head -20
echo
echo '=== driver_elf.o ==='
nm -u build/driver_elf.o | head -10
echo
echo '=== 全量 make（验证真实链接）==='
timeout 500 make build/kernel.bin 2>&1 | tail -25
