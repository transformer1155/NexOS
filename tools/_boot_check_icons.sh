#!/usr/bin/env bash
# Short headless QEMU boot of the BIOS image to verify it still loads the
# SFS (with the new icon TEX files) and reaches the managed shell.
set -u
SERIAL_WIN="D:\\MyOS\\bootloader\\build\\serial_icons.log"
DISK_WIN="D:\\MyOS\\bootloader\\build\\os.img"
rm -f /d/MyOS/bootloader/build/serial_icons.log 2>/dev/null || true
"/d/qemu/qemu-system-x86_64.exe" -m 256 -accel tcg,tb-size=128 \
    -display none -no-reboot \
    -hda "${DISK_WIN}" \
    -serial "file:${SERIAL_WIN}" &
QPID=$!
sleep 30
kill -9 $QPID 2>/dev/null
wait $QPID 2>/dev/null
echo "serial bytes:"
wc -c /d/MyOS/bootloader/build/serial_icons.log 2>/dev/null
echo "--- key markers ---"
grep -nE "mforms: shell|loaded shell\\.mex|K64 fit|kernel|NexOS|CLR\\]|switch_to_64" /d/MyOS/bootloader/build/serial_icons.log | head -30
echo "--- fault scan ---"
grep -nE "fault|triple|panic|GPF|TRIPLE|#PF|cannot" /d/MyOS/bootloader/build/serial_icons.log | head -10
echo "(end)"
