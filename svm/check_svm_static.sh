#!/usr/bin/env bash
# =====================================================================
#  check_svm_static.sh  -  verify the static-only SVM layer
#
#  This is the entire test strategy for the SVM work, deliberately: with
#  no machine capable of nested SVM, correctness has to be established by
#  the compiler, by assertions, and by independent re-derivation -- not by
#  running a hypervisor.
#
#  Checks:
#    1. headers + probe compile for BOTH flavours
#         - host g++        (tests / reporting)
#         - -m32 freestanding -nostdlib  (the real kernel)
#       Every static_assert in the headers must hold or this fails.
#    2. VMCB / NPT / bitmap layout self-check
#    3. NPT builder, verified by an INDEPENDENT page-table walker
#    4. VMCB initialiser, field-by-field against APM2
#    5. svm_probe() reports honestly on THIS host (expected: no SVM)
#
#  Run from WSL:  bash svm/check_svm_static.sh
# =====================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEXROOT="$(cd "$HERE/.." && pwd)"
OUT="/tmp/svm_static"
mkdir -p "$OUT"

KFLAGS="-m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
-fno-pic -fno-pie -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra"

rc=0
fail() { echo "FAIL: $1"; rc=1; }

echo "=== 1a. headers compile as host C++ (all static_asserts must hold) ==="
cat > "$OUT/hdr.cpp" <<'EOF'
#include "svm/svm_vmcb.h"
#include "svm/svm_npt.h"
#include "svm/svm_bitmaps.h"
#include "svm/svm_npt_build.h"
#include "svm/svm_vmcb_init.h"
#include "svm/svm_boot_params.h"
#include "svm/svm_boot_params_build.h"
int probe(){ return (int)(sizeof(struct svm_vmcb)
                        + sizeof(struct svm_iopm)
                        + sizeof(struct svm_msrpm)
                        + sizeof(struct svm_boot_params)); }
EOF
if g++ -std=c++17 -Wall -Wextra -I"$NEXROOT" -c "$OUT/hdr.cpp" -o "$OUT/hdr.o"; then
  echo "  ok  : headers + assertions (host C++17)"
else
  fail "host C++17 header check"
fi

echo "=== 1b. headers compile as 32-bit freestanding kernel C++ ==="
if g++ $KFLAGS -I"$NEXROOT" -c "$OUT/hdr.cpp" -o "$OUT/hdr32.o"; then
  echo "  ok  : headers + assertions (-m32 kernel flags)"
else
  fail "32-bit kernel header check"
fi

echo "=== 1c. all .c files compile as kernel C ==="
for f in svm_probe.c svm_npt_build.c svm_vmcb_init.c svm_boot_params_build.c; do
  printf '  %-26s' "$f"
  if g++ $KFLAGS -I"$HERE" -c "$HERE/$f" -o "$OUT/$(basename $f).o"; then
    echo "ok"
  else
    echo "FAIL"; fail "$f (kernel 32-bit)"
  fi
done

echo "=== 2..5. runtime verifications (host) ==="
host_obj() {
  gcc -std=c11 -Wall -Wextra -I"$HERE" -c "$HERE/$1" -o "$OUT/$2" || return 1
}
host_obj svm_probe.c            probe.o      || fail "svm_probe.c host build"
host_obj svm_npt_build.c        nptb.o       || fail "svm_npt_build.c host build"
host_obj svm_vmcb_init.c        vmcbinit.o   || fail "svm_vmcb_init.c host build"
host_obj svm_boot_params_build.c bpbuild.o   || fail "svm_boot_params_build.c host build"

run_test() {   # $1 = cpp source, $2 = binary name, extra objs...,--  $3.. = objs
  local src="$1"; local bin="$2"; shift 2
  if g++ -std=c++17 -Wall -I"$NEXROOT" -c "$HERE/$src" -o "$OUT/$bin.test.o" \
     && g++ "$OUT/$bin.test.o" "$@" -o "$OUT/$bin"; then
    "$OUT/$bin" || rc=1
  else
    fail "$src build"
  fi
}

echo "--- self-check (layout + probe) ---"
run_test test_svm_static.cpp test_svm_static "$OUT/probe.o"

echo "--- NPT builder (independent walker) ---"
run_test test_npt_build.cpp  test_npt_build  "$OUT/nptb.o"

echo "--- VMCB initialiser (APM2 field-by-field) ---"
run_test test_vmcb_init.cpp  test_vmcb_init  "$OUT/vmcbinit.o"

echo "--- Linux boot zeropage (boot protocol + E820) ---"
run_test test_boot_params.cpp test_boot_params "$OUT/bpbuild.o"

echo
echo "==> overall: $([ $rc -eq 0 ] && echo PASS || echo FAIL)"
exit $rc