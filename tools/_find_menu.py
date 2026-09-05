#!/usr/bin/env python3
"""Locate the light menu box (0xF7F9FC) in a probe shot."""
import sys


def load(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(x) for x in f.readline().split()]
        f.readline()
        return w, h, f.read()


p = sys.argv[1]
w, h, d = load(p)


def is_menu(c):
    return abs(c[0] - 247) < 10 and abs(c[1] - 249) < 10 and abs(c[2] - 252) < 10


# scan for rows that are mostly menu-colour, build a bbox
minx, miny, maxx, maxy = w, h, -1, -1
for y in range(0, h, 2):
    cnt = 0
    for x in range(0, w, 2):
        i = (y * w + x) * 3
        if is_menu(d[i:i + 3]):
            cnt += 1
    if cnt > 8:
        if y < miny: miny = y
        if y > maxy: maxy = y
for x in range(0, w, 2):
    cnt = 0
    for y in range(0, h, 2):
        i = (y * w + x) * 3
        if is_menu(d[i:i + 3]):
            cnt += 1
    if cnt > 8:
        if x < minx: minx = x
        if x > maxx: maxx = x

print("menu-like bbox: x=%d..%d y=%d..%d (w=%d h=%d)" % (minx, maxx, miny, maxy, maxx - minx + 1, maxy - miny + 1))

# also check dark text rows inside that bbox
if maxy >= miny:
    print("dark-text row profile inside menu bbox:")
    for y in range(miny, maxy + 1, 2):
        n = 0
        for x in range(minx, maxx + 1):
            i = (y * w + x) * 3
            if d[i] + d[i + 1] + d[i + 2] < 250:
                n += 1
        if n > 2:
            print("  y=%d dark=%d" % (y, n))
