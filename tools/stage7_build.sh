#!/usr/bin/env bash
set -e
export PATH="/d/MyOS/bootloader/tools:/msys64/usr/bin:/msys64/mingw64/bin:/c/Users/trans/elf_tools/bin:/c/Users/trans/.workbuddy/binaries/python/versions/3.13.12:/c/Program Files/dotnet:$PATH"
cd /d/MyOS/bootloader
rm -f build/linux_sfs.img build/os_textboot.img
tools/build_win.sh textboot
echo "=== images ==="
ls -la build/linux_sfs.img build/os_textboot.img
echo "=== linux_sfs file listing ==="
python3 build/list_sfs.py
