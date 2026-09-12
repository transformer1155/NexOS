#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== splash (final probe set) ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aoE '\[SPLASH\][^[]*' | tail -5
  echo
  echo "=== mforms_boot ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aoE '\[BOOT\] mforms_boot cycles=[0-9]+' | head -2
  echo
  echo "=== faults ==="
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' \
    | grep -aoiE '(fault|panic|exception)' | sort | uniq -c | head
} > build/mfboot.txt 2>&1
