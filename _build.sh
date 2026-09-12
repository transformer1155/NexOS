#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader
touch build/boot.bin build/stage2.bin build/sfs.img build/linux_sfs.img
make \
  CC="cmd.exe /c C:/Users/trans/elf_tools/bin/i686-elf-gcc.exe" \
  LD="cmd.exe /c C:/Users/trans/elf_tools/bin/i686-elf-ld.exe" \
  OBJCOPY="cmd.exe /c C:/Users/trans/elf_tools/bin/i686-elf-objcopy.exe" \
  CC64="cmd.exe /c C:/Users/trans/elf_tools/bin/x86_64-elf-g++.exe" \
  LD64="cmd.exe /c C:/Users/trans/elf_tools/bin/x86_64-elf-ld.exe" \
  OBJCOPY64="cmd.exe /c C:/Users/trans/elf_tools/bin/x86_64-elf-objcopy.exe" \
  build/os_v2.img > /mnt/d/MyOS/bootloader/_build_result.txt 2>&1
