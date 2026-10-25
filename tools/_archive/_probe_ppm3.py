#!/usr/bin/env python3
"""Compare rclick_base vs the frozen Phase-1b frame; locate any light menu rect."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px

w, h, pa = read_ppm("build/rclick_base.ppm")
_, _, pb = read_ppm("build/tb_pin_base.ppm")   # frozen frame (a6919939)
_, _, pc = read_ppm("build/rclick_menu.ppm")   # a6919939 too

def changed(a, b, x0=0, y0=0, x1=None, y1=None):
    x1 = x1 or w
    y1 = y1 or h
    n = 0
    for yy in range(y0, y1):
        row = yy * w * 3
        for xx in range(x0, x1):
            i = row + xx * 3
            if abs(a[i]-b[i]) + abs(a[i+1]-b[i+1]) + abs(a[i+2]-b[i+2]) > 24:
                n += 1
    return n

print("rclick_base vs tb_pin_base (before vs after first rclick):",
      changed(pa, pb))
print("rclick_base vs rclick_menu:", changed(pa, pc))

# Locate light rectangles in the frozen frame (menu is light on dark wall).
def light_regions(px, step=4):
    rows = {}
    for yy in range(0, h, step):
        row = yy * w * 3
        cnt = 0
        for xx in range(0, w, step):
            i = row + xx * 3
            if px[i] + px[i+1] + px[i+2] > 620:
                cnt += 1
        if cnt > 2:
            rows[yy] = cnt
    return rows

lr = light_regions(pb)
if lr:
    ys = sorted(lr)
    print("light rows in frozen frame: y=%d..%d, %d rows" % (ys[0], ys[-1], len(ys)))
    # column scan at the menu's y band
    y0 = ys[0]
    cols = {}
    for xx in range(0, w, 4):
        cnt = 0
        for yy in range(y0, min(h, y0 + 400), 4):
            i = (yy * w + xx) * 3
            if pb[i] + pb[i+1] + pb[i+2] > 620:
                cnt += 1
        if cnt > 2:
            cols[xx] = cnt
    xs = sorted(cols)
    print("light cols in band: x=%d..%d" % (xs[0], xs[-1]))
    print("light band box ~ (%d,%d)-(%d,%d)" % (xs[0], y0, xs[-1], min(h, y0+400)))
else:
    print("NO light region in frozen frame (besides taskbar)")
