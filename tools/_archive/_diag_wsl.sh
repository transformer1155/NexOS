#!/bin/bash
# Diagnostic boot test
BUILD=/mnt/d/MyOS/bootloader/build
cd /tmp
rm -f diag_*

echo "Starting QEMU..."
cat "$BUILD/_diag_mon.txt" | qemu-system-x86_64 \
  -drive format=raw,file="$BUILD/os.img" \
  -m 64M -display none -no-reboot \
  -serial file:/tmp/diag_serial.txt \
  -monitor stdio > /tmp/diag_qemu.log 2>&1

echo "QEMU exit: $?"

echo "--- File Sizes ---"
for f in serial vbe kern vga screen; do
  case $f in
    serial) p=/tmp/diag_serial.txt ;;
    vbe)    p=/tmp/diag_vbe.bin ;;
    kern)   p=/tmp/diag_kern.bin ;;
    vga)    p=/tmp/diag_vga.bin ;;
    screen) p=/tmp/diag_screen.ppm ;;
  esac
  if [ -f "$p" ]; then
    sz=$(wc -c < "$p")
    echo "$f: $sz bytes"
  else
    echo "$f: MISSING"
  fi
done

echo "--- QEMU LOG ---"
cat /tmp/diag_qemu.log 2>/dev/null

echo "--- SERIAL (first 3000 chars) ---"
head -c 3000 /tmp/diag_serial.txt 2>/dev/null

echo ""
echo "--- COPYING ---"
cp /tmp/diag_serial.txt "$BUILD/" 2>/dev/null
cp /tmp/diag_vbe.bin "$BUILD/" 2>/dev/null
cp /tmp/diag_kern.bin "$BUILD/" 2>/dev/null
cp /tmp/diag_vga.bin "$BUILD/" 2>/dev/null
cp /tmp/diag_screen.ppm "$BUILD/" 2>/dev/null
echo DONE
