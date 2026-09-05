#!/usr/bin/env python3
"""Analyze where the non-black pixels are in a captured PPM frame."""
import sys

def load(p):
    f = open(p, "rb")
    assert f.readline().strip() == b"P6"
    w, h = map(int, f.readline().split())
    f.readline()
    return w, h, f.read()

w, h, d = load(sys.argv[1])
rows = {}
for y in range(h):
    n = 0
    ro = y * w * 3
    for x in range(w):
        i = ro + x * 3
        if d[i] or d[i + 1] or d[i + 2]:
            n += 1
    if n:
        rows[y] = n
ys = sorted(rows)
print("non-black rows:", len(ys), "y-range:", (ys[0], ys[-1]) if ys else None)

# per-100px band histogram of non-black pixels
bands = {}
for y, n in rows.items():
    b = y // 100
    bands[b] = bands.get(b, 0) + n
for b in sorted(bands):
    print("  band y=%d..%d: %d px" % (b * 100, b * 100 + 99, bands[b]))

# sample distinct colors with their first position
cols = {}
for y in range(h):
    ro = y * w * 3
    for x in range(w):
        i = ro + x * 3
        if d[i] or d[i + 1] or d[i + 2]:
            c = (d[i], d[i + 1], d[i + 2])
            if c not in cols:
                cols[c] = (x, y)
            if len(cols) >= 15:
                break
    if len(cols) >= 15:
        break
print("sample distinct colors (first-seen position):")
for c, pos in sorted(cols.items(), key=lambda kv: kv[1]):
    print("  RGB%s at %s" % (str(c), str(pos)))
