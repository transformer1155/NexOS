#!/usr/bin/env python3
"""ASCII-render the probe screenshot to see what's really on screen."""
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

w, h, px = read_ppm("build/probe_menu.ppm")
W, H = 100, 36
cols = W
rows = H
out = []
for yy in range(rows):
    line = ""
    for xx in range(cols):
        # average over a block
        x0 = xx * w // cols
        x1 = (xx + 1) * w // cols
        y0 = yy * h // rows
        y1 = (yy + 1) * h // rows
        s = cnt = 0
        for y in range(y0, y1, 2):
            row = y * w * 3
            for x in range(x0, x1, 2):
                i = row + x * 3
                s += px[i] + px[i + 1] + px[i + 2]
                cnt += 1
        v = s / cnt if cnt else 0
        if v > 620:
            line += "#"
        elif v > 480:
            line += "o"
        elif v > 360:
            line += "."
        else:
            line += " "
    out.append(line)
for i, line in enumerate(out):
    print("%3d %s" % (i * h // rows, line))
