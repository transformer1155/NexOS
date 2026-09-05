#!/usr/bin/env python3
"""Text-based sanity check of the baked font8x16: confirm glyph heights are
uniform (~TARGET_H) and widths are reasonable. (Model can't view images.)"""
import re
GUI = r'D:\MyOS\bootloader\gui.cpp'
src = open(GUI, encoding='utf-8', errors='replace').read()
m = re.search(r'const uint8_t font8x16\[256\]\[16\] = \{(.*?)\n\};', src, re.DOTALL)
rows = re.findall(r'\{(0x[0-9A-Fa-f]{2}(?:,0x[0-9A-Fa-f]{2}){15})\}', m.group(1))
font = [[int(x,16) for x in r.split(',')] for r in rows]

def stats(cp):
    g = font[cp]
    top = 16; bot = -1; left = 8; right = -1
    for r in range(16):
        b = g[r]
        for c in range(8):
            if b & (0x80 >> c):
                if r < top: top = r
                if r > bot: bot = r
                if c < left: left = c
                if c > right: right = c
    h = (bot - top + 1) if bot >= 0 else 0
    w = (right - left + 1) if right >= 0 else 0
    return h, w

chars = [chr(c) for c in range(0x20,0x7F)]
hs = []; ws = []
bad = []
for ch in chars:
    h, w = stats(ord(ch))
    hs.append(h); ws.append(w)
    if h < 12 or h > 15:
        bad.append((ch, h, w))
print(f"glyphs={len(chars)}")
print(f"height: min={min(hs)} max={max(hs)} avg={sum(hs)/len(hs):.1f}")
print(f"width : min={min(ws)} max={max(ws)} avg={sum(ws)/len(ws):.1f}")
print("height outside 12..15:", bad if bad else "none")
# show a few wide ones
for ch in 'MWilm.':
    h,w = stats(ord(ch))
    print(f"  '{ch}': h={h} w={w}")
