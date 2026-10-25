#!/usr/bin/env python3
"""Render the new font8x16 (Segoe UI, corrected) to a montage PNG for
visual confirmation that glyph proportions are natural (not crushed)."""
import re, struct
from PIL import Image

GUI = r'D:\MyOS\bootloader\gui.cpp'
with open(GUI, 'r', encoding='utf-8', errors='replace') as f:
    src = f.read()

m = re.search(r'const uint8_t font8x16\[256\]\[16\] = \{(.*?)\n\};', src, re.DOTALL)
rows = re.findall(r'\{(0x[0-9A-Fa-f]{2}(?:,0x[0-9A-Fa-f]{2}){15})\}', m.group(1))
assert len(rows) >= 128, len(rows)
font = [[int(x,16) for x in r.split(',')] for r in rows]

def glyph_img(cp, scale=4, pad=2):
    img = Image.new('L', (8*scale + pad*2, 16*scale + pad*2), 0)
    g = font[cp]
    for row in range(16):
        b = g[row]
        for col in range(8):
            if b & (0x80 >> col):
                for yy in range(scale):
                    for xx in range(scale):
                        img.putpixel((pad + col*scale + xx, pad + row*scale + yy), 255)
    return img

# characters to show: A-Z, a-z, 0-9, and some punctuation
chars = []
chars += [chr(c) for c in range(0x41, 0x5B)]   # A-Z
chars += [chr(c) for c in range(0x61, 0x7B)]   # a-z
chars += [chr(c) for c in range(0x30, 0x3A)]   # 0-9
chars += list('.,:;!?@#$%&*()[]{}<>-+/=')
per_row = 16
scale = 4
pad = 2
rows_n = (len(chars) + per_row - 1) // per_row
cell_w = 8*scale + pad*2
cell_h = 16*scale + pad*2
gap = 6
W = per_row*cell_w + (per_row-1)*gap + 20
H = rows_n*cell_h + (rows_n-1)*gap + 20
canvas = Image.new('L', (W, H), 30)  # dark grey bg
for i, ch in enumerate(chars):
    cp = ord(ch)
    if cp >= 256: continue
    g = glyph_img(cp, scale, pad)
    r = i // per_row
    c = i % per_row
    x = 10 + c*(cell_w + gap)
    y = 10 + r*(cell_h + gap)
    canvas.paste(g, (x, y))
canvas = canvas.convert('RGB')
canvas.save(r'D:\MyOS\bootloader\_font_segoe_strip.png')
print('wrote _font_segoe_strip.png', canvas.size, 'chars', len(chars))
