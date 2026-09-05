#!/usr/bin/env bash
# debug.sh - inspect CPU state, loaded memory regions and VGA after boot.
cd "$(dirname "$0")"
IMG="build/os.img"
rm -f build/vga_debug.bin build/qemu_debug.log
(
  sleep 3
  echo "info registers"
  echo "echo --- 0x7c00 ---"
  echo "xp/16bx 0x7c00"
  echo "echo --- 0x8000 ---"
  echo "xp/16bx 0x8000"
  echo "echo --- 0x10000 ---"
  echo "xp/16bx 0x10000"
  echo "memsave 0xb8000 0x1000 build/vga_debug.bin"
  sleep 1
  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M -display none \
        -no-reboot -d cpu_reset,guest_errors -D build/qemu_debug.log \
        -monitor stdio 2>&1 | tee build/debug.log
echo
echo "=== QEMU debug log (resets/errors) ==="
sed -n '1,30p' build/qemu_debug.log
echo
echo "=== VGA ==="
python3 -c "
d=open('build/vga_debug.bin','rb').read()
print(bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')[:400])
" 2>/dev/null || echo "(no vga dump)"
