#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== cursor backend + display audit ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aE 'CURSOR|INTEL|SPLASH|Entered GUI|panic|fault|triple' | head -20
} > build/cursor_check.txt 2>&1
