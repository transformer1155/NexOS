#!/usr/bin/env bash
# Build the mc_launcher guest ELF32 (Minecraft-style remote-desktop demo) using
# the same freestanding i686-elf toolchain + flags as the kernel's `python`
# target.  Output: linux_root/mc_launcher (picked up by sfs_gen -> linux_sfs.img).
set -e
cd "$(dirname "$0")/.."

TOOLBIN=/c/Users/trans/elf_tools/bin
export PATH="$TOOLBIN:$PATH"

CC=i686-elf-g++
LD=i686-elf-ld
CFLAGS="-x c -m32 -ffreestanding -nostdlib -fno-stack-protector -fno-pic \
-fno-pie -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr"

mkdir -p build
"$CC" $CFLAGS -c usr/libc.c -o build/mc_libc.o
"$CC" $CFLAGS -c usr/mc_launcher.c -o build/mc_launcher.o
"$LD" -m elf_i386 -nostdlib -Ttext=0x08048000 -e _start \
      build/mc_libc.o build/mc_launcher.o -o linux_root/mc_launcher

echo "==> linux_root/mc_launcher built ($(stat -c%s linux_root/mc_launcher) bytes)"
