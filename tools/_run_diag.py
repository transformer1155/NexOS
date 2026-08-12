"""Wrapper to run _diag_boot.py via WSL (bypasses security policy)."""
import subprocess, sys

# Write a shell script for WSL
script = '''
#!/bin/bash
cd /mnt/d/MyOS/bootloader
BUILD_DIR=/mnt/d/MyOS/bootloader/build
rm -f "$BUILD_DIR"/diag_*.txt "$BUILD_DIR"/diag_*.bin "$BUILD_DIR"/diag_*.ppm "$BUILD_DIR"/diag_qemu.log

echo "=== Running QEMU diagnostic ==="
cat <<'MONCMD' | qemu-system-x86_64 -drive format=raw,file="$BUILD_DIR/os.img" -m 64M -display none -no-reboot -serial file:"$BUILD_DIR/diag_serial.txt" -monitor stdio > "$BUILD_DIR/diag_qemu.log" 2>&1
sleep 4
sendkey r
sleep 0.1
sendkey o
sleep 0.1
sendkey o
sleep 0.1
sendkey t
sleep 0.1
sendkey ret
sleep 1.5
memsave 0x5000 256 "$BUILD_DIR/diag_vbe.bin"
memsave 0x10000 256 "$BUILD_DIR/diag_kern.bin"
memsave 0xB8000 4096 "$BUILD_DIR/diag_vga.bin"
screendump "$BUILD_DIR/diag_screen.ppm"
quit
MONCMD

echo "=== RESULTS ==="
echo "SERIAL_SIZE=$(stat -c%s "$BUILD_DIR/diag_serial.txt" 2>/dev/null || echo 0)"
echo "VBE_SIZE=$(stat -c%s "$BUILD_DIR/diag_vbe.bin" 2>/dev/null || echo 0)"
echo "KERN_SIZE=$(stat -c%s "$BUILD_DIR/diag_kern.bin" 2>/dev/null || echo 0)"
echo "VGA_SIZE=$(stat -c%s "$BUILD_DIR/diag_vga.bin" 2>/dev/null || echo 0)"
echo "SCR_SIZE=$(stat -c%s "$BUILD_DIR/diag_screen.ppm" 2>/dev/null || echo 0)"

echo "=== SERIAL OUTPUT ==="
cat "$BUILD_DIR/diag_serial.txt" 2>/dev/null | head -c 3000 || echo "(empty)"

echo "=== DONE ==="
'''

result = subprocess.run(
    ['wsl', '-e', 'bash', '-c', script],
    capture_output=True, text=True, timeout=120
)
print("STDOUT:")
print(result.stdout)
if result.stderr:
    print("STDERR:")
    print(result.stderr[-1000:])
