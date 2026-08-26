#!/usr/bin/env bash
# Pure-Windows NexOS build wrapper (NO WSL).
#
# Invoke via MSYS2's bash, e.g. from PowerShell:
#   & "C:\msys64\usr\bin\bash.exe" -lc 'cd /d/MyOS/bootloader && tools/build_win.sh textboot'
#
# PATH = MSYS2 coreutils/make/nasm + i686-elf cross compiler + managed Python
#        + .NET SDK.  The i686-elf bin must come before anything else named
#        g++/ld/objcopy so the Makefile's bare `g++`/`ld`/`objcopy` resolve to
#        the ELF cross tools.
#
# CC/LD/OBJCOPY are passed on the make COMMAND LINE (not env) because the
# Makefile assigns them with `:=`, which would override plain environment
# variables.  CC64/LD64 are command-line overrides too: the long-mode kernel
# needs a separate x86_64-elf compiler (the Makefile only has CC64 ?= $(CC)).
export PATH="/d/MyOS/bootloader/tools:/msys64/usr/bin:/msys64/mingw64/bin:/c/Users/trans/elf_tools/bin:/c/Users/trans/.workbuddy/binaries/python/versions/3.13.12:/c/Program Files/dotnet:$PATH"
cd "$(dirname "$0")/.." || exit 1
exec make \
  CC="i686-elf-g++" \
  CC64="x86_64-elf-g++" \
  LD="i686-elf-ld" \
  LD64="x86_64-elf-ld" \
  OBJCOPY="i686-elf-objcopy" \
  OBJCOPY64="x86_64-elf-objcopy" \
  "$@"
