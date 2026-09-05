#!/bin/bash
set -e
cd /mnt/d/MyOS/bootloader
echo "=== clean (remove stale mingw-built .o that would clash with ELF) ==="
make clean >/dev/null 2>&1 || true
echo "=== full build with WSL g++/ld (ELF) ==="
make 2>&1 | tail -40
echo "BUILD_EXIT=${PIPESTATUS[0]}"
ls -la os_v2.img 2>/dev/null || echo "no os_v2.img produced"
