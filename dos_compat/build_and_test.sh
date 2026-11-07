#!/usr/bin/env bash
# =====================================================================
#  build_and_test.sh  -  dos_compat build + logic verification
#
#  Compiles the INT 21h translation layer against the REAL NexOS VFS
#  (built off-kernel with -DVFS_HOST_TEST) plus the host DOS primitives.
#  This exercises the genuine overlay logic: SFS stays read-only, RAMFS
#  handles create/write, and vfs_create is wired correctly.
#
#  Run from WSL:  bash build_and_test.sh
# =====================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEXROOT="$(cd "$HERE/.." && pwd)"      # NexOS/ — where vfs.h / proc.h live
OUT="$HERE/out"
mkdir -p "$OUT"

echo "==> building real VFS + DOS layer (host link)"
set -x
g++ -std=c++17 -Wall -Wextra -DVFS_HOST_TEST -I"$NEXROOT" -I"$HERE" -c "$NEXROOT/vfs.cpp" -o "$OUT/vfs_real.o" || exit 1
g++ -std=c++17 -Wall -Wextra -DVFS_HOST_TEST -I"$NEXROOT" -I"$HERE" -c "$HERE/vfs_host_shim.cpp" -o "$OUT/vfs_host_shim.o" || exit 1
g++ -std=c++17 -Wall -Wextra -DDOS_HOST -I"$HERE" -I"$NEXROOT" -c "$HERE/dos_prim_host.c" -o "$OUT/dos_prim_host.o" || exit 1
g++ -std=c++17 -Wall -Wextra -DDOS_HOST -I"$HERE" -I"$NEXROOT" -c "$HERE/dos_int21.c"    -o "$OUT/dos_int21.o"    || exit 1

for t in test_ah09 test_ah_file; do
  g++ -std=c++17 -Wall -Wextra -DDOS_HOST -I"$HERE" -I"$NEXROOT" -c "$HERE/$t.c" -o "$OUT/$t.o" || exit 1
  g++ "$OUT/dos_prim_host.o" "$OUT/dos_int21.o" "$OUT/$t.o" "$OUT/vfs_real.o" "$OUT/vfs_host_shim.o" -o "$OUT/$t" || exit 1
done
set +x

rc=0
for t in test_ah09 test_ah_file; do
  echo
  echo "==> running $t (against REAL vfs.cpp)"
  "$OUT/$t" || rc=1
done
echo
echo "==> overall: $([ $rc -eq 0 ] && echo PASS || echo FAIL)"
exit $rc