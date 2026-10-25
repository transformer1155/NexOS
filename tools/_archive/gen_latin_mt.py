#!/usr/bin/env python3
"""Bake the Latin subset of Microsoft YaHei into a multi-resolution grayscale
bitmap font pack for NexOS (the "Y" stage: quasi-vector, runtime size pick).

Output: sfs_files/font_la.bin
  Binary layout:
    "FLT1"            (4 bytes magic)
    uint8  nsizes
    int16  sizes[nsizes]            (e.g. 12,16,20,28)
    uint8  ncp
    per codepoint (only 0x20..0x7E):
        uint8  cp
        per size:
            uint8  w            (glyph pixel width, height == size)
            uint8  bitmap[w*size]   (row-major alpha 0..255, top-down)

The kernel loads this from SFS at GUI init, picks the nearest size for the
current font pixel height, and alpha-blends the glyph (no 1-bit pixels, no
hard scaling artifacts).  This is the "quasi-vector" path; the full
true-vector rasterizer (stb_truetype / custom) is the later "X" stage.
"""
import os, sys, struct
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, 'sfs_files', 'msyh_sub.ttf')   # subset we just built
OUT  = os.path.join(ROOT, 'sfs_files', 'font_la.bin')
SIZES = [12, 16, 20, 28]

def main():
    if not os.path.exists(SRC):
        # fall back to the full system font if subset missing
        SRC2 = r'C:\Windows\Fonts\msyh.ttc'
        print("subset not found, using", SRC2)
        font_src, idx = SRC2, 0
    else:
        font_src, idx = SRC, 0

    fonts = {s: ImageFont.truetype(font_src, s, index=idx) for s in SIZES}

    codepoints = list(range(0x20, 0x7F))
    out = bytearray()
    out += b'FLT1'
    out += struct.pack('<B', len(SIZES))
    for s in SIZES:
        out += struct.pack('<h', s)
    out += struct.pack('<B', len(codepoints))

    for cp in codepoints:
        ch = chr(cp)
        out += struct.pack('<B', cp)
        for s in SIZES:
            f = fonts[s]
            # render at size s, crop to ink, record width
            img = Image.new('L', (s * 2, s * 2), 0)
            d = ImageDraw.Draw(img)
            d.text((2, 2), ch, font=f, fill=255)
            bbox = img.getbbox()
            if bbox is None:
                out += struct.pack('<B', 0)
                continue
            ink = img.crop(bbox)
            iw, ih = ink.size
            # width cap: keep aspect but allow up to s (no overflow)
            w = min(iw, s)
            # re-crop to w width from right (proportional) -- simpler: scale to w x s
            scl = ink.resize((w, s), Image.LANCZOS)
            out += struct.pack('<B', w)
            for row in range(s):
                for col in range(w):
                    out.append(scl.getpixel((col, row)))

    with open(OUT, 'wb') as f:
        f.write(out)
    print("Wrote", OUT, "(%d bytes)" % len(out))

if __name__ == '__main__':
    main()
