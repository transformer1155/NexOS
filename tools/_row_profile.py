#!/usr/bin/env python3
"""Row-profile a probe shot's menu region: count dark pixels per row."""
import sys


def load(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(x) for x in f.readline().split()]
        f.readline()
        return w, h, f.read()


p = sys.argv[1]
w, h, d = load(p)
x0, y0, x1 = 60, 60, 320
print("== %s rows %d..%d (dark px per row in x=%d..%d)" % (p, y0, y0 + 420, x0, x1))
for y in range(y0, y0 + 430, 2):
    n = 0
    for x in range(x0, x1):
        i = (y * w + x) * 3
        if d[i] + d[i + 1] + d[i + 2] < 250:
            n += 1
    if n > 0:
        print("y=%3d dark=%3d  %s" % (y, n, "#" * min(n, 40)))
