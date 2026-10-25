#!/usr/bin/env python3
r"""
gen_latin_font.py - Re-bake the kernel's 8x16 Latin bitmap font from
Microsoft Segoe UI, while preserving the original CP437 glyphs in the
0x00-0x1F and 0x7F-0xFF ranges (box-drawing / legacy symbols).

Output: patches D:\MyOS\bootloader\gui.cpp in place, replacing the
`const uint8_t font8x16[256][16] = { ... };` literal block.
"""
import struct, os, re, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI = os.path.join(ROOT, 'gui.cpp')

# ---- Microsoft YaHei (微软雅黑) candidates (pure Windows first) ----
CANDIDATES = [
    r'C:\Windows\Fonts\msyh.ttc',       # index 0 = 常规
    r'C:\Windows\Fonts\msyhbd.ttc',     # 粗体 fallback
    '/mnt/c/Windows/Fonts/msyh.ttc',
    '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc',
]

def find_font():
    for c in CANDIDATES:
        if os.path.exists(c):
            return c
    return None

def render_glyph(ch, font):
    """Render a char into an 8x16 mono bitmap (1=pixel). Returns 16 bytes.

    Fixed, uniform glyph HEIGHT (TARGET_H) so on-screen text is consistent
    and fills the 16px cell instead of looking tiny/inconsistent.  Width is
    kept proportional to the ink but capped at the 8px cell; only genuinely
    wide glyphs (W/M) get mild horizontal compression -- the normal look of
    a proportional bitmap font.
    """
    CELL_W, CELL_H = 8, 16
    TARGET_H = 14                      # consistent, readable height
    src = Image.new('L', (40, 40), 0)
    d = ImageDraw.Draw(src)
    try:
        d.text((4, 6), ch, font=font, fill=255)
    except Exception:
        return bytes(16)
    bbox = src.getbbox()
    if bbox is None:
        return bytes(16)
    ink = src.crop(bbox)
    iw, ih = ink.size
    if ih == 0 or iw == 0:
        return bytes(16)
    th = TARGET_H
    tw = max(1, int(round(iw * th / ih)))
    tw = min(tw, CELL_W)               # cap width; wide glyphs compress mildly
    scl = ink.resize((tw, th), Image.LANCZOS)
    out = Image.new('L', (CELL_W, CELL_H), 0)
    ox = (CELL_W - tw) // 2
    oy = (CELL_H - th) // 2            # vertical centering -> uniform baseline
    out.paste(scl, (ox, oy))
    glyph = bytearray(16)
    for row in range(CELL_H):
        byte = 0
        for col in range(CELL_W):
            v = out.getpixel((col, row))
            if v > 96:
                byte |= 0x80 >> col
        glyph[row] = byte
    return bytes(glyph)

def parse_existing(block):
    """Parse the existing font8x16 array literal into 256 bytearrays
    (the source only initializes 0x00-0x7F; high half is zero-filled)."""
    rows = re.findall(r'\{(0x[0-9A-Fa-f]{2}(?:,0x[0-9A-Fa-f]{2}){15})\}', block)
    assert len(rows) >= 128, "expected >=128 rows, got %d" % len(rows)
    out = [bytearray(16) for _ in range(256)]
    for i, r in enumerate(rows):
        nums = [int(x, 16) for x in r.split(',')]
        out[i] = bytearray(nums)
    return out

def main():
    fp = find_font()
    if not fp:
        print("No Microsoft YaHei font found."); sys.exit(1)
    print("Latin font:", fp)
    # msyh.ttc is a TrueType Collection: index 0 = regular YaHei.
    try:
        font = ImageFont.truetype(fp, 22, index=0)
    except TypeError:
        font = ImageFont.truetype(fp, 22)

    with open(GUI, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()

    m = re.search(r'const uint8_t font8x16\[256\]\[16\] = \{(.*?)\n\};', src, re.DOTALL)
    if not m:
        print("font8x16 block not found in gui.cpp"); sys.exit(1)
    existing = parse_existing(m.group(1))

    # Replace printable ASCII 0x20..0x7E with Microsoft YaHei; keep legacy ranges.
    for cp in range(0x20, 0x7F):
        existing[cp] = bytearray(render_glyph(chr(cp), font))

    # Emit new block, one row per line (matches original style).
    lines = ['const uint8_t font8x16[256][16] = {']
    for i in range(256):
        row = ','.join('0x%02X' % b for b in existing[i])
        comma = ',' if i < 255 else ''
        lines.append('    {%s}%s' % (row, comma))
    lines.append('};')
    new_block = '\n'.join(lines)

    src2 = src[:m.start()] + new_block + src[m.end():]
    with open(GUI, 'w', encoding='utf-8') as f:
        f.write(src2)
    print("Patched gui.cpp font8x16 (0x20-0x7E = Microsoft YaHei, legacy preserved).")

if __name__ == '__main__':
    main()
