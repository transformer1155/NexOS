#!/usr/bin/env bash
# usage: logdump.sh <file> [maxlines]
cd /mnt/d/MyOS/bootloader || exit 1
F="${1:-build/boot64_serial.log}"
N="${2:-200}"
{
  echo "=== $F (size $(stat -c%s "$F" 2>/dev/null)) ==="
  tr -c '[:print:]\n' ' ' < "$F" | tr -s ' ' | grep -v '^ *$' | head -"$N"
} > build/logdump.txt 2>&1
