#!/usr/bin/env python3
"""Check wallpaper brightness and search for a menu rect in the probe shot."""
import os
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# 1. wallpaper brightness
im = Image.open("assets/wallpaper.png").convert("RGB").resize((640, 360), Image.LANCZOS)
px = im.load()
n = light = s = 0
for yy in range(0, im.height, 2):
    for xx in range(0, im.width, 2):
        r, g, b = px[xx, yy]
        s += r + g + b
        n += 1
        if r + g + b > 620:
            light += 1
print("wallpaper.png avg=%d light>620=%d%%" % (s // n, 100 * light // n))

# 2. probe_menu.ppm middle band
def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px

w, h, px = read_ppm("build/probe_menu.ppm")
# light row profile y=300..670
rows = []
for yy in range(280, 680, 4):
    row = yy * w * 3
    cnt = 0
    for xx in range(0, w, 4):
        i = row + xx * 3
        if px[i] + px[i + 1] + px[i + 2] > 620:
            cnt += 1
    rows.append((yy, cnt))
# print only rows with significant light (potential menu)
sig = [(y, c) for y, c in rows if c > 8]
print("significant light rows y=280..676:", sig[:40] if sig else "NONE")
# col profile in any found band
if sig:
    y0 = sig[0][0]
    cols = []
    for xx in range(0, w, 4):
        cnt = 0
        for yy in range(y0, min(y0 + 320, 676), 4):
            i = (yy * w + xx) * 3
            if px[i] + px[i + 1] + px[i + 2] > 620:
                cnt += 1
        if cnt > 4:
            cols.append(xx)
    if cols:
        print("col span x=%d..%d" % (cols[0], cols[-1]))
