#!/usr/bin/env python3
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
x0, x1 = 260, 800
print("y-row rightmost non-white in x[%d,%d] for typed snapshot:" % (x0, x1))
for y in range(125, 175):
    right = -1
    color = None
    for x in range(x1, x0, -1):
        o = (y*w+x)*3
        if not (px[o]>250 and px[o+1]>250 and px[o+2]>250):
            right = x; color = (px[o], px[o+1], px[o+2]); break
    if right > 0:
        print("  y=%d right=%d color=%s" % (y, right, color))
