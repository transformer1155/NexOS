#!/usr/bin/env bash
export PATH="$PATH:/mnt/c/Users/trans/elf_tools/bin"
export GCC_EXEC_PREFIX="C:/Users/trans/elf_tools/"
printf 'int main(){}' > /tmp/t.cpp
i686-elf-gcc.exe -m32 -v -c /tmp/t.cpp -o /tmp/t.o 2>&1 | tail -50
