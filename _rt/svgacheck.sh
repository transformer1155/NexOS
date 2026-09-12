#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== [SVGA] / [CURSOR] / [DGOP] / [GUI] ==="
  tr -c '[:print:]\n' ' ' < build/svga_serial.log | tr -s ' ' \
    | grep -aoE '\[SVGA\][^[]*|\[CURSOR\][^[]*|\[DGOP\][^[]*|\[SPLASH\] active[^[]*' | head -40
  echo
  echo "=== faults ==="
  tr -c '[:print:]\n' ' ' < build/svga_serial.log | tr -s ' ' \
    | grep -aoiE 'fault|panic|exception|triple' | sort | uniq -c | head
  echo
  echo "=== tail ==="
  tr -c '[:print:]\n' ' ' < build/svga_serial.log | tr -s ' ' | tail -4
  echo
  echo "=== shot ==="
  ls -l build/svga_shot.ppm 2>&1
  echo "=== qemu stderr ==="
  tail -3 build/svga.err 2>&1
} > build/svga_check.txt 2>&1
