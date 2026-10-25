#!/usr/bin/env python3
"""
Comprehensive boot diagnostic: checks serial, VGA memory, kernel memory, screendump.
Runs QEMU inside WSL.
"""
import subprocess, sys, os, time, struct

WSL_BASE = '/mnt/d/MyOS/bootloader'
BUILD_DIR = WSL_BASE + '/build'

SERIAL_LOG = BUILD_DIR + '/diag_serial.txt'
VGA_DUMP   = BUILD_DIR + '/diag_vga.bin'
VBE_DUMP   = BUILD_DIR + '/diag_vbe.bin'
KERN_DUMP  = BUILD_DIR + '/diag_kern.bin'
SCR_DUMP   = BUILD_DIR + '/diag_screen.ppm'

# Build QEMU command line
# We use 'sleep' in monitor to wait, then dump everything
qemu_script = f'''
cd {WSL_BASE}
rm -f {SERIAL_LOG} {VGA_DUMP} {VBE_DUMP} {KERN_DUMP} {SCR_DUMP} build/diag_qemu.log

# Run QEMU with monitor commands piped in
cat <<'MONCMD' | qemu-system-x86_64 -drive format=raw,file={BUILD_DIR}/os.img -m 64M -display none -no-reboot -serial file:{SERIAL_LOG} -monitor stdio > build/diag_qemu.log 2>&1
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
sleep 1
memsave 0x5000 256 {VBE_DUMP}
memsave 0x10000 256 {KERN_DUMP}
memsave 0xB8000 4096 {VGA_DUMP}
screendump {SCR_DUMP}
quit
MONCMD

echo "=== DIAG QEMU DONE ==="
echo "SERIAL_SIZE=$(stat -c%s {SERIAL_LOG} 2>/dev/null || echo 0)"
echo "VGA_SIZE=$(stat -c%s {VGA_DUMP} 2>/dev/null || echo 0)"
echo "VBE_SIZE=$(stat -c%s {VBE_DUMP} 2>/dev/null || echo 0)"
echo "KERN_SIZE=$(stat -c%s {KERN_DUMP} 2>/dev/null || echo 0)"
echo "SCR_SIZE=$(stat -c%s {SCR_DUMP} 2>/dev/null || echo 0)"
'''

print("Running QEMU diagnostic inside WSL...")
result = subprocess.run(
    ['wsl', '-e', 'bash', '-lc', qemu_script],
    capture_output=True, text=True, timeout=120
)
print("WSL stdout:", result.stdout[-1000:])
if result.stderr:
    print("WSL stderr:", result.stderr[-500:])

# --- Now analyze from Windows side ---
WIN_BUILD = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'build')
print("\n" + "="*60)
print("DIAGNOSTIC RESULTS")
print("="*60)

# 1. Serial output
print("\n--- Serial Output ---")
ser_path = os.path.join(WIN_BUILD, 'diag_serial.txt')
if os.path.exists(ser_path) and os.path.getsize(ser_path) > 0:
    with open(ser_path, 'rb') as f:
        data = f.read()
    print(f"Serial file size: {len(data)} bytes")
    text = data.decode('latin-1', errors='replace')
    # Show first 2000 chars
    display = text[:2000].replace('\x00', '')
    print(f"Content:\n{display}")
    # Check markers
    markers = ['[K1]', '[K2]', '[K3]', '[K4]', '[K5]', '[K6]', '[K7]', '[K8]',
               '[HW]', 'VBE', 'PMM', 'VMM', 'HEAP',
               'V', 'B', 'E', 'F', 'Q', 'G',  'N',  # Stage2 VBE markers
               'Hello', 'NexOS', 'Shell']
    print("\nMarker analysis:")
    for m in markers:
        found = False
        if m.isascii():
            found = m.encode('latin-1') in data
        else:
            found = m in text
        print(f"  {'[FOUND]' if found else '[MISS]'} {m}")
else:
    print("Serial file EMPTY or MISSING")

# 2. VBE info at 0x5000
print("\n--- VBE Info Block (0x5000) ---")
vbe_path = os.path.join(WIN_BUILD, 'diag_vbe.bin')
if os.path.exists(vbe_path) and os.path.getsize(vbe_path) > 0:
    with open(vbe_path, 'rb') as f:
        data = f.read()
    if all(b == 0 for b in data):
        print("ALL ZEROS - VBE info not set by Stage2")
    elif all(b == 0xFF for b in data):
        print("ALL 0xFF - memory inaccessible")
    else:
        fb_phys = struct.unpack('<I', data[0:4])[0]
        width = struct.unpack('<H', data[4:6])[0]
        height = struct.unpack('<H', data[6:8])[0]
        bpp = data[8]
        pitch = struct.unpack('<H', data[9:11])[0]
        mode = struct.unpack('<H', data[11:13])[0]
        vbe_ok = data[13]
        mode_set = data[14]
        px_fmt = data[15]
        print(f"fb_phys=0x{fb_phys:08X} width={width} height={height} bpp={bpp} pitch={pitch}")
        print(f"mode=0x{mode:04X} vbe_ok={vbe_ok} mode_set={mode_set} px_fmt={px_fmt}")
        print(f"Raw[0:32]: {' '.join(f'{b:02x}' for b in data[:32])}")
else:
    print("VBE dump MISSING or EMPTY")

# 3. Kernel code at 0x10000
print("\n--- Kernel Code (0x10000) ---")
kern_path = os.path.join(WIN_BUILD, 'diag_kern.bin')
kernel_bin = os.path.join(WIN_BUILD, 'kernel.bin')
if os.path.exists(kern_path) and os.path.getsize(kern_path) > 0:
    with open(kern_path, 'rb') as f:
        data = f.read()
    if all(b == 0 for b in data):
        print("ALL ZEROS - kernel NOT LOADED!")
    elif all(b == 0xFF for b in data):
        print("ALL 0xFF - memory inaccessible")
    else:
        print(f"Non-zero data found ({len(data)} bytes)")
        print(f"Raw[0:32]: {' '.join(f'{b:02x}' for b in data[:32])}")
        if os.path.exists(kernel_bin):
            with open(kernel_bin, 'rb') as f2:
                expected = f2.read(256)
            match = all(data[i] == expected[i] for i in range(min(256, len(data))))
            if match:
                print("KERNEL LOADED CORRECTLY (matches kernel.bin)")
            else:
                for i in range(min(256, len(data))):
                    if data[i] != expected[i]:
                        print(f"FIRST MISMATCH at offset {i}: got 0x{data[i]:02X}, expected 0x{expected[i]:02X}")
                        break
        else:
            print("(cannot compare: kernel.bin not found)")
else:
    print("Kernel dump MISSING or EMPTY")

# 4. VGA text buffer at 0xB8000
print("\n--- VGA Text Buffer (0xB8000) ---")
vga_path = os.path.join(WIN_BUILD, 'diag_vga.bin')
if os.path.exists(vga_path) and os.path.getsize(vga_path) > 0:
    with open(vga_path, 'rb') as f:
        data = f.read()
    if all(b == 0 for b in data):
        print("ALL ZEROS - no text written (kernel not running/applied?)")
    elif all(b == 0xFF for b in data):
        print("ALL 0xFF - memory inaccessible")
    else:
        # Decode as VGA text (char+attribute pairs)
        chars = []
        for i in range(0, min(len(data), 4000), 2):
            ch = data[i]
            attr = data[i+1] if i+1 < len(data) else 0
            if ch == 0:
                chars.append(' ')
            elif 32 <= ch < 127:
                chars.append(chr(ch))
            else:
                chars.append('.')
        text = ''.join(chars)
        print("Visible content (first 10 lines):")
        for ln in range(10):
            line = text[ln*80:(ln+1)*80].rstrip()
            if line.strip():
                print(f"  [{ln}]: {line}")
        print(f"Raw[0:64]: {' '.join(f'{b:02x}' for b in data[:64])}")
else:
    print("VGA dump MISSING or EMPTY")

# 5. Screendump
print("\n--- Screendump ---")
scr_path = os.path.join(WIN_BUILD, 'diag_screen.ppm')
if os.path.exists(scr_path) and os.path.getsize(scr_path) > 0:
    with open(scr_path, 'rb') as f:
        data = f.read()
    print(f"Size: {len(data)} bytes")
    try:
        nl1 = data.index(b'\n')
        magic = data[:nl1].strip().decode()
        idx = nl1 + 1
        while idx < len(data) and data[idx:idx+1] == b'#':
            idx = data.index(b'\n', idx) + 1
        nl2 = data.index(b'\n', idx)
        w, h = map(int, data[idx:nl2].strip().split())
        idx = nl2 + 1
        nl3 = data.index(b'\n', idx)
        maxval = int(data[idx:nl3].strip())
        idx = nl3 + 1
        pixels = data[idx:]
        print(f"Format: {magic} {w}x{h} maxval={maxval} pixels={len(pixels)}")
        non_black = sum(1 for i in range(0, len(pixels)-2, 3) 
                       if pixels[i] > 30 or pixels[i+1] > 30 or pixels[i+2] > 30)
        total = max(len(pixels) // 3, 1)
        print(f"Non-black: {non_black}/{total} ({100*non_black//total}%)")
        if non_black > 100:
            print("SCREEN HAS VISIBLE CONTENT!")
        else:
            print("SCREEN IS BLANK (all black pixels)")
    except Exception as e:
        print(f"PPM parse failed: {e}")
else:
    print("Screendump MISSING or EMPTY")

print("\n" + "="*60)
print("DIAGNOSTIC COMPLETE")
print("="*60)
