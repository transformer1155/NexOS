#!/bin/bash
cd /mnt/d/MyOS/bootloader
LD=/mnt/c/Users/trans/elf_tools/bin/i686-elf-ld.exe
OD=/mnt/c/Users/trans/elf_tools/bin/i686-elf-objdump.exe
$LD -m elf_i386 -nostdlib -T linker.ld -z noexecstack -Map build/kernel32.map -o /tmp/k32.elf \
  build/entry.o build/switch32to64.o build/kernel.o build/divdi3.o build/ai_engine.o build/ai_plugin.o \
  build/kb.o build/skill.o build/gguf.o build/net.o build/distnet.o build/gui.o build/font_vec.o \
  build/addrman.o build/winloader.o build/win32.o build/linux_compat.o build/gdt.o build/syscall.o \
  build/proc.o build/vfs.o build/perm.o build/clr.o build/mforms.o 2>/tmp/ld.err
echo "ld exit: $?"
echo "--- function + instructions around 4be22 ---"
$OD -d /tmp/k32.elf 2>/dev/null | awk '
/^[0-9a-f]+ <[a-zA-Z_][a-zA-Z0-9_]*>:/{nm=$0; cnt=0}
/4be22:/{print "FUNC: " nm; print; cnt=6}
cnt>0 && !/^[0-9a-f]+ <[a-zA-Z_][a-zA-Z0-9_]*>:/{print; cnt--}'
echo "--- map: symbols near 4be22 ---"
grep -iE '0x0*4be22|0x0*4be[01][0-9a-f]' build/kernel32.map | head
