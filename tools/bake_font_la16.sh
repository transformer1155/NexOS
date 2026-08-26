#!/usr/bin/env bash
# Bake sfs_files/font_la16.bin: rasterize msyh.ttf ASCII 0x20-0x7F at 16px
# into a 16x16 grayscale (coverage) bitmap, using the verified stb_truetype
# engine, by booting a tiny bare-metal 64-bit "bake kernel" under QEMU and
# capturing the glyph bytes over the serial port.  No host C compiler / network
# required (the x86_64-elf cross-compiler and qemu are already in the tree).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
export PATH="/c/Users/trans/elf_tools/bin:/msys64/usr/bin:$PATH"
CC64="x86_64-elf-g++"
LD64="x86_64-elf-ld"
OBJCOPY64="x86_64-elf-objcopy"
QEMU="D:/qemu/qemu-system-x86_64.exe"
TTF="sfs_files/msyh.ttf"
OUT="sfs_files/font_la16.bin"

set -e
echo "[bake] compiling bake kernel..."
"$CC64" -m64 -ffreestanding -nostdlib -fno-exceptions -fno-rtti -fno-pic -fno-pie \
        -fcf-protection=none -fno-asynchronous-unwind-tables -O2 \
        -I tools/bake_inc -I tools/stb -I tools/vecmath \
        -c tools/bake_la16_kernel.c -o /tmp/bake_la16.o

echo "[bake] embedding ${TTF} ..."
"$OBJCOPY64" -I binary -O elf64-x86-64 -B i386:x86-64 "$TTF" /tmp/msyh_ttf.o

echo "[bake] linking bake.elf ..."
"$LD64" -e _start -Ttext 0x100000 -o /tmp/bake_la16.elf /tmp/bake_la16.o /tmp/msyh_ttf.o

echo "[bake] booting under qemu, capturing serial ..."
rm -f /tmp/bake_out.bin /tmp/bake_out.log
( sleep 8; echo "quit" ) | "$QEMU" -kernel /tmp/bake_la16.elf -serial file:/tmp/bake_out.bin \
     -display none -no-reboot -monitor stdio >/tmp/bake_out.log 2>&1 || true

echo "[bake] parsing serial dump -> ${OUT} ..."
python - "$OUT" <<'PY'
import sys, re
out = sys.argv[1]
data = open('/tmp/bake_out.bin','rb').read().decode('latin1', 'replace')
m = re.search(r'BAKE_START(.*?)BAKE_END', data, re.S)
if not m:
    sys.exit("BAKE_START/BAKE_END not found in serial dump")
body = m.group(1).strip().split('\n')
glyphs = b''
hdr = b'LA16' + bytes([0x20, 96, 16 & 0xFF, 0])  # base,count,height(LE)
count = 0
for line in body:
    line = line.strip()
    if not line: continue
    toks = line.split()
    if len(toks) < 3: continue
    cp = int(toks[0],16); w=int(toks[1],16); h=int(toks[2],16)
    expect = w*h
    vals = toks[3:] if len(toks) > 3 else []
    if len(vals) != expect:
        sys.exit("glyph 0x%02X: expected %d bytes, got %d" % (cp, expect, len(vals)))
    row = bytes(int(v,16) for v in vals)
    # pad/trim to 16x16, width byte + h*h grayscale (h==16)
    glyphs += bytes([w]) + row[:16*16]
    count += 1
if count != 96:
    sys.exit("expected 96 glyphs, got %d" % count)
open(out,'wb').write(hdr + glyphs)
print("baked %d glyphs -> %s (%d bytes)" % (count, out, len(hdr)+len(glyphs)))
PY
echo "[bake] done."
