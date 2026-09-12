#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== pixel format lines ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aE 'Pixel format|BGRX|RGBX|RGB565|fmt=|bpp=|VBE:' | head -10
} > build/fmt.txt 2>&1
