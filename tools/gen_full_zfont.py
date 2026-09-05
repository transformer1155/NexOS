#!/usr/bin/env python3
"""
gen_full_zfont.py - Generate a near-complete CJK 16x16 dot-matrix font for NexOS.

Covers essentially ALL common + rare Simplified/Traditional Hanzi:
  * CJK Extension A : U+3400 .. U+4DBF
  * CJK Unified     : U+4E00 .. U+9FFF
(about 27,500 code points).  Glyphs the source TTF cannot render (blank/tofu)
are skipped, so the output holds only real glyphs.

Output: sfs_files/zfont.bin  (ZFN2 format, Unicode-sorted)

  Offset 0 : 'ZFN2'            (4 bytes)
  Offset 4 : uint16 count (LE)
  Offset 6 : uint16 unicode[count]   (ascending, for binary search)
  Followed : uint8  glyph[count][32] (16 rows x 16 cols, MSB-first), parallel

The kernel loads this from SFS at GUI init and repoints its renderer to it.
The embedded 387-char font (zfont_data.h) remains as an early-boot fallback.

Usage:
  python tools/gen_full_zfont.py [output_path]
"""
import struct, os, sys

# ---- CJK ranges to cover (inclusive) ----
RANGES = [
    (0x3400, 0x4DBF),   # CJK Extension A
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs
]

def render_glyph(ch, font):
    """Render a char into a 16x16 mono bitmap (1=pixel). Returns 32 bytes or None."""
    from PIL import Image, ImageDraw
    src = Image.new('L', (22, 22), 0)
    d = ImageDraw.Draw(src)
    try:
        d.text((0, -1), ch, font=font, fill=255)
    except Exception:
        return None
    img = src.resize((16, 16), Image.LANCZOS)
    # Reject essentially-blank glyphs (tofu / missing in the font).
    nonzero = 0
    glyph = bytearray(32)
    for row in range(16):
        for half in range(2):
            byte = 0
            for b in range(8):
                col = half * 8 + b
                v = img.getpixel((col, row))
                if v > 64:
                    byte |= 0x80 >> b
                    nonzero += 1
            glyph[row * 2 + half] = byte
    if nonzero < 3:          # almost blank -> not a usable glyph
        return None
    return bytes(glyph)

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join('sfs_files', 'zfont.bin')
    # Optional explicit font path (argv[2]); else fall back to candidates.
    font_path = sys.argv[2] if len(sys.argv) > 2 else None

    # Font candidates (pure Windows: direct C:\Windows\Fonts paths first)
    candidates = [
        'C:\\Windows\\Fonts\\msyh.ttc',
        'C:\\Windows\\Fonts\\msyhbd.ttc',
        'C:\\Windows\\Fonts\\simsun.ttc',
        'C:\\Windows\\Fonts\\simhei.ttf',
        'C:\\Windows\\Fonts\\simsunb.ttf',
        '/mnt/c/Windows/Fonts/msyh.ttc',
        '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc',
        '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
    ]
    if not font_path:
        for c in candidates:
            if os.path.exists(c):
                font_path = c
                break
    if not font_path:
        print("No CJK font found. Install MSYH/SimSun/Noto."); sys.exit(1)
    print("Font:", font_path)

    from PIL import ImageFont
    try:
        font = ImageFont.truetype(font_path, 16)
    except Exception:
        # .ttc needs an index
        font = ImageFont.truetype(font_path, 16, index=0)

    items = []   # list of (unicode_codepoint, glyph_bytes)
    total = 0
    rendered = 0
    skipped = 0
    for lo, hi in RANGES:
        for cp in range(lo, hi + 1):
            total += 1
            try:
                ch = chr(cp)
            except Exception:
                skipped += 1
                continue
            g = render_glyph(ch, font)
            if g is None:
                skipped += 1
                continue
            items.append((cp, g))
            rendered += 1
    print("Scanned %d code points; rendered %d; skipped %d" % (total, rendered, skipped))

    # Sort by Unicode codepoint (required for the kernel's binary search).
    items.sort(key=lambda t: t[0])
    count = len(items)

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(b'ZFN2')
        f.write(struct.pack('<H', count))
        for cp, _ in items:
            f.write(struct.pack('<H', cp))
        for _, g in items:
            f.write(g)

    size = 6 + count * (2 + 32)
    print("Wrote %s : %d glyphs, %d bytes (%.1f KB)" % (out_path, count, size, size / 1024.0))

if __name__ == '__main__':
    main()
