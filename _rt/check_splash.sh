#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== serial markers ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aE 'SPLASH|Entered GUI|lock screen|panic|fault|triple' | head -12
  echo "=== convert frames ==="
  python3 _rt/ppm2png.py build/splash_t01.ppm build/splash_t02.ppm build/splash_t03.ppm \
      build/splash_t05.ppm build/splash_t40.ppm 2>&1 | tail -7
} > build/splash_check.txt 2>&1
