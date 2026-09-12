#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== SVGA / CURSOR ==="
  tr -c '[:print:]\n' ' ' < build/svga_serial.log | tr -s ' ' \
    | grep -aoE '\[SVGA\][^[]*|\[CURSOR\][^[]*|\[DGOP\][^[]*' | head -20
  echo
  echo "=== faults (lines, not counts) ==="
  tr -c '[:print:]\n' ' ' < build/svga_serial.log | tr -s ' ' \
    | grep -aoiE '.{0,30}(fault|panic|exception).{0,30}' | head -6
  echo
  echo "=== shot ==="
  ls -l build/svga_shot.ppm 2>&1
  echo "=== convert ==="
  python3 _rt/ppm2png.py build/svga_shot.ppm 2>&1 | tail -2
} > build/svga_final.txt 2>&1
