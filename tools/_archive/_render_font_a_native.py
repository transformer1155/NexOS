#!/usr/bin/env python3
"""Render the new grayscale 12x24 Latin font (gui_font_a.h) at native res
to a PNG strip so we can eyeball anti-aliasing quality."""
import re, struct
src = open('gui_font_a.h', encoding='utf-8').read()
# parse widths
wblock = re.search(r'g_fontA_w\[128\] = \{(.*?)\};', src, re.DOTALL).group(1)
widths = [int(x) for x in re.findall(r'\d+', wblock)]
# parse data (everything after 'static const uint8_t g_fontA[')
dblock = re.search(r'g_fontA\[\d+\] = \{(.*?)\n\};', src, re.DOTALL).group(1)
vals = [int(x) for x in re.findall(r'\d+', dblock)]
GW, GH = 12, 24
# build strip
chars = [chr(c) for c in range(0x20, 0x7F)]
cols = 16
rows = (len(chars) + cols - 1) // cols
pad = 4
W = cols * (GW + pad) + pad
H = rows * (GH + pad) + pad
img = bytearray(W * H * 3)
def setpx(x, y, r, g, b):
    if 0 <= x < W and 0 <= y < H:
        i = (y * W + x) * 3
        img[i] = r; img[i+1] = g; img[i+2] = b
# white bg
for y in range(H):
    for x in range(W):
        setpx(x, y, 235, 235, 240)
# dark text with alpha (anti-aliased over light bg)
for ri, ch in enumerate(chars):
    c = ord(ch)
    cx = pad + (ri % cols) * (GW + pad)
    cy = pad + (ri // cols) * (GH + pad)
    base = c * GW * GH
    for r in range(GH):
        for col in range(GW):
            a = vals[base + r*GW + col]
            # blend dark text over light bg
            fr, fg, fb = 20, 20, 28
            br, bgc, bb = 235, 235, 240
            r2 = (br*(255-a) + fr*a)//255
            g2 = (bgc*(255-a) + fg*a)//255
            b2 = (bb*(255-a) + fb*a)//255
            setpx(cx+col, cy+r, r2, g2, b2)
with open('_font_a_native.png', 'wb') as f:
    # minimal PPM -> use PNG via manual? write PPM then convert is messy;
    # instead write a simple BMP.
    # write BMP 24-bit
    filesize = 54 + W*H*3
    f.write(b'BM')
    f.write(struct.pack('<I', filesize))
    f.write(b'\x00\x00\x00\x00')
    f.write(struct.pack('<I', 54))
    f.write(struct.pack('<I', 40))
    f.write(struct.pack('<i', W))
    f.write(struct.pack('<i', H))
    f.write(struct.pack('<H', 1))
    f.write(struct.pack('<H', 24))
    f.write(struct.pack('<I', 0))
    f.write(struct.pack('<I', W*H*3))
    f.write(struct.pack('<i', 2835))
    f.write(struct.pack('<i', 2835))
    f.write(struct.pack('<I', 0))
    f.write(struct.pack('<I', 0))
    # BMP rows bottom-up
    for y in range(H-1, -1, -1):
        for x in range(W):
            i = (y*W+x)*3
            f.write(bytes([img[i+2], img[i+1], img[i]]))
print("wrote _font_a_native.png", W, H)
