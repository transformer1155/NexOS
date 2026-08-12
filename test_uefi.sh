#!/usr/bin/env bash
# =====================================================================
#  test_uefi.sh  -  UEFI boot test with QEMU + OVMF
# ---------------------------------------------------------------------
#  Usage: ./test_uefi.sh <image> [iso]
#    <image>  - UEFI-bootable image (os_uefi.img, os_hybrid.img, or os.iso)
#    [iso]    - optional: if "iso", boot from CD-ROM instead of hard disk
#
#  Boots the UEFI image, captures serial output (UEFI + kernel debug),
#  takes a screen dump (screendump PPM), and verifies boot milestones.
#
#  NOTE: QEMU's monitor "pmemsave"/"xp" at 0xB8000 returns 0xFFFFFFFF
#  after ExitBootServices under OVMF, because the firmware's memory
#  mapping is torn down.  We therefore verify VGA output via the
#  QEMU "screendump" command (rendered framebuffer) and count non-black
#  pixels as evidence that text is visible on screen.
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"

SRC="${1:-build/os_uefi.img}"
MODE="${2:-disk}"
OUT="build/uefi_test.log"
SER="build/serial_uefi.txt"
SCR="build/screen.ppm"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"
OVMF_VARS="/usr/share/OVMF/OVMF_VARS.fd"

rm -f "$OUT" "$SER" "$SCR"
cp "$OVMF_VARS" build/ovmf_vars_test.fd

# Build QEMU drive arguments based on mode
if [ "$MODE" = "iso" ]; then
  DRIVE_ARGS="-cdrom $SRC"
  echo "==> Booting UEFI ISO with QEMU + OVMF ..."
else
  DRIVE_ARGS="-drive format=raw,file=$SRC"
  echo "==> Booting UEFI disk image with QEMU + OVMF ..."
fi

# Run QEMU with serial captured to file, monitor to stdin/stdout
(
  sleep 8                          # wait for OVMF + bootloader + kernel
  echo "screendump $SCR"           # save rendered screen as PPM
  sleep 1
  echo "quit"
) | qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/ovmf_vars_test.fd \
    $DRIVE_ARGS \
    -m 128M -display none -no-reboot \
    -serial file:"$SER" \
    -monitor stdio > "$OUT" 2>&1 || true

echo
echo "===== Serial Output ====="
if [ -s "$SER" ]; then
  cat "$SER"
else
  echo "(no serial output)"
fi

echo
echo "===== Screen Dump Analysis ====="
if [ -s "$SCR" ]; then
  echo "Screen dump saved: $SCR ($(stat -c%s "$SCR") bytes)"
  python3 -c "
data = open('$SCR','rb').read()
# Parse PPM header (newline-separated: P6\n<w> <h>\n<maxval>\n<pixels>)
nl1 = data.index(b'\n')
magic = data[:nl1].strip()
idx = nl1 + 1
nl2 = data.index(b'\n', idx)
w, h = map(int, data[idx:nl2].strip().split())
idx = nl2 + 1
nl3 = data.index(b'\n', idx)
maxval = int(data[idx:nl3].strip())
idx = nl3 + 1
pixels = data[idx:]
print(f'PPM: {magic.decode()} {w}x{h} maxval={maxval} ({len(pixels)} bytes pixel data)')
non_black = 0
for i in range(0, len(pixels) - 2, 3):
    if pixels[i] > 30 or pixels[i+1] > 30 or pixels[i+2] > 30:
        non_black += 1
total = len(pixels) // 3
pct = 100 * non_black // max(total, 1)
print(f'Non-black pixels: {non_black}/{total} ({pct}%)')
if non_black > 100:
    print('Screen has content (text visible)')
else:
    print('Screen appears blank')
" 2>/dev/null || echo "(PPM parse failed)"
else
  echo "(no screen dump)"
fi

echo
echo "===== Assertions ====="
python3 - "$SER" "$SCR" <<'PY'
import sys, os

ser  = open(sys.argv[1], 'rb').read().decode('latin-1', errors='replace') \
       if os.path.getsize(sys.argv[1]) > 0 else ""
scr_path = sys.argv[2]

ok = True
def chk(cond, msg):
    global ok
    status = "PASS" if cond else "FAIL"
    print(f"{status}: {msg}")
    if not cond:
        ok = False

# ---- UEFI bootloader messages on serial ----
chk("[UEFI] UEFI bootloader"  in ser, "UEFI bootloader started")
chk("Using embedded kernel"   in ser, "kernel.bin loaded by UEFI bootloader")
chk("Kernel copied to 0x10000" in ser, "kernel copied to 0x10000")
chk("Exiting boot services"   in ser, "ExitBootServices called")

# ---- Mode-transition debug markers on serial ----
# E = enter_kernel entered, G = GDT loaded, I = IDT loaded, 3 = 32-bit compat mode,
# S = _start reached, F = FPU enabled, B = BSS zeroed
chk("EGI3SFB" in ser, "enter_kernel: full mode transition + BSS zero (E->G->I->3->S->F->B)")

# ---- Kernel debug markers (optional - may be garbled by VGA mode switch) ----
# Check for kernel entry markers in serial output
chk("[K1]" in ser,  "kmain entered")
chk("[K2]" in ser,  "VGA text mode set")
chk("[K3]" in ser,  "terminal init done")
chk("[K4]" in ser,  "mouse init done")
chk("[K5]" in ser,  "Hello world written to terminal")

# ---- VGA screen content (via screendump PPM) ----
# QEMU monitor pmemsave at 0xB8000 returns 0xFFFFFFFF under OVMF, so
# we verify the rendered framebuffer has non-black pixels (text).
non_black = 0
if os.path.getsize(scr_path) > 0:
    data = open(scr_path, 'rb').read()
    # Skip PPM header (3 lines)
    idx = 0
    for _ in range(3):
        idx = data.index(b'\n', idx) + 1
    pixels = data[idx:]
    for i in range(0, len(pixels) - 2, 3):
        if pixels[i] > 30 or pixels[i+1] > 30 or pixels[i+2] > 30:
            non_black += 1

chk(non_black > 100,
    f"VGA screen shows content ({non_black} non-black pixels in screendump)")

if ok:
    print("\n=== ALL TESTS PASSED ===")
else:
    print("\n=== SOME TESTS FAILED ===")
sys.exit(0 if ok else 1)
PY
