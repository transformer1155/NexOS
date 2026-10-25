#!/usr/bin/env python3
"""Compare tray_base/tray_menu from the test run; locate the difference."""
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

import hashlib
for a, b in [("tray_base", "tray_menu"), ("tb_bar_base", "tb_bar_menu")]:
    pa, pb = "build/%s.ppm" % a, "build/%s.ppm" % b
    if not (os.path.exists(pa) and os.path.exists(pb)):
        print(a, "missing"); continue
    w, h, p1 = read_ppm(pa)
    _, _, p2 = read_ppm(pb)
    md5a = hashlib.md5(p1).hexdigest()[:8]
    md5b = hashlib.md5(p2).hexdigest()[:8]
    x0, y0, x1, y1 = w, h, -1, -1
    n = 0
    for yy in range(h):
        row = yy * w * 3
        for xx in range(w):
            i = row + xx * 3
            if (abs(p1[i]-p2[i]) + abs(p1[i+1]-p2[i+1]) + abs(p1[i+2]-p2[i+2])) > 24:
                n += 1
                if xx < x0: x0 = xx
                if xx > x1: x1 = xx
                if yy < y0: y0 = yy
                if yy > y1: y1 = yy
    print("%s vs %s md5=%s/%s changed=%d box=(%d,%d)-(%d,%d)" % (a, b, md5a, md5b, n, x0, y0, x1, y1))
