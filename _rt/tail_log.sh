#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== size ==="
  stat -c%s build/verify_gui.log
  echo "=== fault markers ==="
  tr -c '[:print:]\n' ' ' < build/verify_gui.log | tr -s ' ' \
    | grep -aE 'panic|fault|triple|Exception|abort|Page|#PF|#GP' | head -10
  echo "=== last 12 printable lines ==="
  tr -c '[:print:]\n' ' ' < build/verify_gui.log | tr -s ' ' | tail -12
} > build/logtail.txt 2>&1
