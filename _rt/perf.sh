#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== splash render cost ($1) ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aE 'SPLASH|CURSOR' | head -12
} > build/perf.txt 2>&1
