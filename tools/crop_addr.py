#!/usr/bin/env python3
import sys
from PIL import Image

def load(fn):
    d = open(fn, "rb").read()
    assert d[:2] == b"P6"
    i = 2; vals = []
    while len(vals) < 3:
        while d[i] in b" \t\r\n": i += 1
        if d[i:i+1] == b"#":
            while d[i] not in b"\r\n": i += 1
            continue
        j = i
        while d[j] not in b" \t\r\n": j += 1
        vals.append(int(d[i:j])); i = j
    i += 1; w, h, mx = vals
    return w, h, d[i:i+w*h*3]

# analyze address band: report rightmost dark column per snapshot
TXT_X0, TXT_Y0, TXT_X1, TXT_Y1 = 246, 33, 780, 51
OX, OY = 42, 106  # client origin on screen
for n in ["1_focus", "2_typed", "3_bs1", "4_bs2"]:
    w, h, px = load("build/addr_32_%s.ppm" % n)
    x0 = OX + TXT_X0; x1 = min(OX + TXT_X1, w)
    y0 = OY + TXT_Y0; y1 = min(OY + TXT_Y1, h)
    right_old = -1
    right_new = -1
    for x in range(x0, x1):
        for y in range(y0, y1):
            o = (y*w + x)*3
            if px[o] < 100 and px[o+1] < 100 and px[o+2] < 100:
                right_old = x; break
        for y in range(y0, y1):
            o = (y*w + x)*3
            if max(px[o], px[o+1], px[o+2]) < 200:
                right_new = x; break
    # also crop a wider band for visual
    row0, row1 = 110, 180
    crop = bytearray()
    for y in range(row0, row1):
        crop += px[y*w*3:(y+1)*w*3]
    im = Image.frombytes("RGB", (w, row1-row0), bytes(crop))
    im.save("build/addr_32_%s_crop.png" % n)
    print("%-10s right_edge_old=%d right_edge_new=%d (x0=%d x1=%d)" % (n, right_old, right_new, x0, x1))
