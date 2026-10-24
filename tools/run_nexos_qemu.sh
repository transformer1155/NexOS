#!/bin/bash
# Boot os_v2.img under QEMU (i386 + EPYC: EFER + SVM both emulated), capture
# the COM1 log, and report the [VMM] diagnostic lines.
cd /mnt/e/NexOS/NexOS || exit 1
LOG=${1:-/tmp/nexos_serial.log}
TIMEOUT=${2:-90}

timeout "$TIMEOUT" qemu-system-i386 \
    -drive format=raw,file=build/os_v2.img \
    -cpu EPYC \
    -m 4096 \
    -display none \
    -serial "file:$LOG" \
    -no-reboot >/dev/null 2>&1

echo "--- [VMM] lines from serial log ---"
grep -a "\[VMM\]" "$LOG" | head -40
echo "--- tail of log ---"
tail -c 400 "$LOG"
echo "[--- end ---]"
