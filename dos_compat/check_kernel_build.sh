#!/usr/bin/env bash
# =====================================================================
#  check_kernel_build.sh  -  compile the DOS layer with the REAL kernel
#                            flags used by the NexOS Makefile
#
#  This isolates the DOS compatibility files from the full `make` so a
#  syntax/flags regression is caught in seconds instead of after a long
#  kernel link.  It uses exactly the Makefile's CXXFLAGS.
#
#  Run from WSL:  bash check_kernel_build.sh
# =====================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEXROOT="$(cd "$HERE/.." && pwd)"

CXXFLAGS="-m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
-fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra \
-fpermissive -I$NEXROOT -I$HERE"

FILES="
dos_compat/dos_prim_kernel.cpp
dos_compat/dos_int21.c
dos_compat/dos_int21_isr.cpp
dos_compat/dos_psp.c
dos_compat/dos_exec.c
"

rc=0
cd "$NEXROOT"
for f in $FILES; do
  printf '%-40s' "$f"
  obj="/tmp/kb_$(basename "$f").o"
  if g++ $CXXFLAGS -c "$f" -o "$obj" 2>/tmp/kb_err.txt; then
    echo "OK"
  else
    echo "FAIL"
    sed 's/^/    /' /tmp/kb_err.txt
    rc=1
  fi
done

# Also verify the kernel translation unit that registers the INT 21h gate.
printf '%-40s' "kernel.cpp (INT 21h gate)"
if g++ $CXXFLAGS -c kernel.cpp -o /tmp/kb_kernel.o 2>/tmp/kb_err.txt; then
  echo "OK"
else
  echo "FAIL"
  sed 's/^/    /' /tmp/kb_err.txt
  rc=1
fi

echo
echo "==> kernel-flags compile: $([ $rc -eq 0 ] && echo PASS || echo FAIL)"
exit $rc