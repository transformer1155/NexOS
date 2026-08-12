#!/usr/bin/env python3
"""Analyse probe PPM shots: menu rect, window region, brightness stats."""
import sys


def load(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(x) for x in f.readline().split()]
        f.readline()
        return w, h, f.read()


def region_stats(w, h, d, x0, y0, x1, y1, label):
    vals = []
    for y in range(max(0, y0), min(h, y1), 4):
        for x in range(max(0, x0), min(w, x1), 4):
            i = (y * w + x) * 3
            vals.append(d[i] + d[i + 1] + d[i + 2])
    if not vals:
        print("%-34s empty" % label)
        return
    dark = 100.0 * sum(1 for v in vals if v < 250) / len(vals)
    print("%-34s avg=%4d dark%%=%.1f" % (label, sum(vals) // len(vals), dark))


for p in sys.argv[1:]:
    try:
        w, h, d = load(p)
    except Exception as e:
        print(p, "LOAD FAIL", e)
        continue
    print("== %s  %dx%d" % (p, w, h))
    region_stats(w, h, d, 68, 68, 252, 454, "menu-region 68,68-252,454")
    region_stats(w, h, d, 380, 222, 900, 582, "fexplorer win 380,222-900,582")
    region_stats(w, h, d, 640, 360, 1280, 660, "screen right/top area")
