#!/usr/bin/env bash
export PATH="$PATH:/mnt/c/Users/trans/elf_tools/bin"
{
  echo "which_gcc: $(which i686-elf-gcc)"
  echo "--- libexec dir ---"
  ls -la /mnt/c/Users/trans/elf_tools/libexec/gcc/i686-elf/ 2>&1
  echo "--- bin cc1plus? ---"
  ls -la /mnt/c/Users/trans/elf_tools/bin/cc1plus* 2>&1
  echo "--- try trivial compile ---"
  printf 'int main(){}' > /tmp/t.cpp
  i686-elf-gcc -m32 -c /tmp/t.cpp -o /tmp/t.o 2>&1
  echo "compile_exit=$?"
} > /mnt/d/MyOS/bootloader/_cc_test.txt 2>&1
