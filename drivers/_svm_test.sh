#!/bin/bash
# ============================================================================
# _svm_test.sh - Build and run SVM driver host test
# ----------------------------------------------------------------------------
# All artifacts go in drivers/ (NOT /tmp — WSL sessions lose /tmp).
# If svm_driver.c is missing we still compile the harness against it and report.
# ==========================================================================*/
set -u
cd "$(dirname "$0")"

DRV_SRC="svm_driver.c"
DRV_OBJ="svm.o"
DRV_IMG="svm.drv"
TEST_BIN="svmtest"

echo "=== _svm_test.sh  start  ==="
echo "cwd = $(pwd)"

# --- Step 1: Check svm_driver.c exists -----------------------------------
if [ ! -f "$DRV_SRC" ]; then
    echo ""
    echo "WARNING: $DRV_SRC not found — svm-porter has not delivered it yet."
    echo "Compilation below will fail; we'll report and exit cleanly."
    echo ""
fi

# --- Step 2: Compile driver as freestanding ELF32 -----------------------
echo ""
echo "[1/3] compile  $DRV_SRC -> $DRV_OBJ"
gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector     -Os -I. -c "$DRV_SRC" -o "$DRV_OBJ" 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "COMPILE FAILED (rc=$RC). Is svm_driver.c present and valid C32?"
    echo "Aborting before link."
    exit 3
fi
echo "  -> OK ($(stat -c%s "$DRV_OBJ" 2>/dev/null || stat -f%z "$DRV_OBJ" 2>/dev/null) bytes)"

# --- Step 3: Link driver object into .drv image -------------------------
echo ""
echo "[2/3] link     $DRV_OBJ -> $DRV_IMG"
ld -m elf_i386 -r -o "$DRV_IMG" "$DRV_OBJ" 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "LINK FAILED (rc=$RC)"
    exit 4
fi
echo "  -> OK ($(stat -c%s "$DRV_IMG" 2>/dev/null || stat -f%z "$DRV_IMG" 2>/dev/null) bytes)"

# --- Step 4: Compile harness and link with driver_loader + driver_elf ---
echo ""
echo "[3/3] build + run  $TEST_BIN"
gcc -m32 -O0 -w -I. -o "$TEST_BIN" _svm_test.c driver_loader.c driver_elf.c 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "HARNESS BUILD FAILED (rc=$RC)"
    exit 5
fi

echo ""
echo "========================================================================"
echo "RUNNING  ./$TEST_BIN ./$DRV_IMG"
echo "========================================================================"
./"$TEST_BIN" "./$DRV_IMG"
RC=$?
echo ""
echo "driver test exited rc=$RC"

echo ""
echo "=== _svm_test.sh  done  ==="
exit $RC
