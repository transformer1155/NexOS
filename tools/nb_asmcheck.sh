#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
nasm -f elf64 .attic64/entry64.asm -o /tmp/entry64.o 2>&1 | head -30
echo "=== nasm rc=$? ==="
ls -l /tmp/entry64.o 2>/dev/null
