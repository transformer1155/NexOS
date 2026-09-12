#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== cadence / cost ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aoE '\[SPLASH\][^[]*' | tail -8
} > build/cadence.txt 2>&1
