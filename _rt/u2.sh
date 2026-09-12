#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== lines mentioning mcopy/mformat/esp ==="
  grep -n -E 'mcopy|mformat|mmd|esp\.img|os_uefi' /tmp/u2.txt
  echo "=== last 15 lines ==="
  tail -15 /tmp/u2.txt
} > build/u2_extract.txt 2>&1
