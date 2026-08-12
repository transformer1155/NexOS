#!/bin/sh
# =====================================================================
#  host_w32_test.sh - build + run the Win32 subsystem on the build host
# ---------------------------------------------------------------------
#  Links win32.cpp into a static, freestanding 32-bit Linux binary so the
#  PE32 loader can be tested without building an ISO and booting QEMU.
#  No 32-bit libc (gcc-multilib) is required - the harness talks to the
#  kernel through int 0x80 directly.
# =====================================================================
set -e
cd "$(dirname "$0")/.."

OUT=build/host_w32_test
mkdir -p build

g++ -m32 -std=c++17 -O1 -g \
    -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic \
    -fno-builtin -Wall -Wextra -Wno-unused-parameter \
    -DW32_HOSTTEST \
    -nostdlib -nostartfiles -static \
    -Wl,-melf_i386 -Wl,-z,execstack -Wl,-z,noexecheap -Wl,--build-id=none \
    -o "$OUT" tools/host_w32_test.cpp win32.cpp 2>&1 | grep -v 'noexecheap' || true

if [ ! -x "$OUT" ]; then
    # retry without the optional -z noexecheap (not supported by every ld)
    g++ -m32 -std=c++17 -O1 -g \
        -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic \
        -fno-builtin -Wall -Wextra -Wno-unused-parameter \
        -DW32_HOSTTEST \
        -nostdlib -nostartfiles -static \
        -Wl,-melf_i386 -Wl,-z,execstack -Wl,--build-id=none \
        -o "$OUT" tools/host_w32_test.cpp win32.cpp
fi

echo "built $OUT"
exec "$OUT" "${1:-hello32.exe}" "${2:-sfs_files/}"
