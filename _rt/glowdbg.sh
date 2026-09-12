#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' | grep -a 'GLOW' | head -8
  echo "--- render cost ---"
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' | grep -a 'render cost' | head -2
  tr -c '[:print:]\n' ' ' < build/splash_serial.log | tr -s ' ' | grep -a 'static split' | head -2
} > build/glow_dbg.txt 2>&1
