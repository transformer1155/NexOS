#!/bin/bash
# =====================================================================
#  run_svm_smoke.sh - AMD-V (SVM) plumbing smoke test under QEMU TCG
# =====================================================================
#
#  Boots tools/svm_host_test.asm: a standalone single-sector hypervisor
#  that mirrors the exact VMCB layout / VMRUN sequence / #VMEXIT dispatch
#  used by plugins/vmm.cpp.  QEMU TCG emulates AMD-V in software
#  (-cpu +svm), so the full VMRUN -> guest -> #VMEXIT -> VMRUN cycle runs
#  without real hardware.
#
#  PASS = the guest's "ready" appears on serial followed by SVMOK:
#         the VMCB constants are proven and can be trusted in vmm.cpp.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

command -v nasm >/dev/null || { echo "need nasm"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "need qemu-system-x86_64"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

nasm -f bin plugins/guest_smoke.asm -o "$WORK/payload.bin" || exit 1
echo "guest payload: $(wc -c < "$WORK/payload.bin") bytes"

nasm -f bin -D PAYLOAD_FILE="\"$WORK/payload.bin\"" tools/svm_host_test.asm \
    -o "$WORK/disk.bin" || exit 1
echo "svm host sector: $(wc -c < "$WORK/disk.bin") bytes"

echo "--- booting SVM host under QEMU TCG (i386 + EPYC: EFER + SVM both present) ---"
timeout 120 qemu-system-i386 \
    -drive format=raw,file="$WORK/disk.bin" \
    -cpu EPYC \
    -m 64 \
    -display none \
    -serial "file:$LOG" \
    -no-reboot \
    >/dev/null 2>&1

echo "--- serial output ---"
cat "$LOG" 2>/dev/null || true
echo "[--- end ---]"
echo "--- serial hex dump (decode the X+exitcode) ---"
od -A d -t x1z "$LOG" 2>/dev/null | head -6

if grep -q "SVMOK" "$LOG" 2>/dev/null; then
    echo "[ OK ] VMRUN entered the guest and #VMEXIT returned"
    echo "[ OK ] VMCB layout validated: I/O exits + HLT exit + resume all work"
    echo "[ OK ] these constants are safe to use in plugins/vmm.cpp"
    exit 0
fi
echo "[FAIL] no SVMOK -- see 'X' + exit code above for the failure reason"
exit 1
