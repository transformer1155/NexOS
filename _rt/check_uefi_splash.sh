#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== UEFI boot log: splash + key markers ==="
  tr -c '[:print:]\n' ' ' < build/uefi_boot.log | tr -s ' ' \
    | grep -aE 'NexOS UEFI|Loading kernel|SFS staged|GOP:|Graphics OK|RAM-backed image|SPLASH|zfont|Framebuffer init|Entered GUI|lock screen armed|panic|fault|Exception' \
    | head -30
} > build/uefi_splash_check.txt 2>&1
