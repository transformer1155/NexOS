#!/usr/bin/env python3
import sys

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

w, h, px = load("build/addr_32_2_typed.ppm")
# sample along the text baseline y=147 for x=520..620
y = 147
print("pixels at y=%d:" % y)
for x in range(500, 620, 5):
    o = (y*w+x)*3
    print("  x=%d  RGB=(%3d,%3d,%3d)" % (x, px[o], px[o+1], px[o+2]))

# find rightmost non-white column in a wide y band
for y0, y1 in [(130, 170), (139, 157)]:
    right = -1
    for x in range(260, 900):
        for y in range(y0, y1):
            o = (y*w+x)*3
            if not (px[o]>250 and px[o+1]>250 and px[o+2]>250):
                right = x; break
    print("y[%d,%d] rightmost non-white = %d" % (y0, y1, right))
