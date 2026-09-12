#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  make -p 2>/dev/null | grep -E '^(MFORMAT|MCOPY|MMD|OVMF_CODE|OVMF_VARS|WSL|MFORMAT )' 
  echo "--- direct command -v ---"
  command -v mformat mcopy mmd
} > build/vars.txt 2>&1
cat build/vars.txt
