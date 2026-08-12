#!/usr/bin/env python3
"""Diff two probe shots; report bounding box of changed pixels + menu check."""
import sys


def load(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(x) for x in f.readline().split()]
        f.readline()
        return w, h, f.read()


def diff_bbox(a, b, tol=24):
    w, h, pa = a
    _, _, pb = b
    x0, y0, x1, y1 = w, h, 0, 0
    n = 0
    for y in range(0, h):
        rowa = y * w * 3
        for x in range(0, w):
            i = rowa + x * 3
            if (abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1])
                    + abs(pa[i + 2] - pb[i + 2])) > tol:
                n += 1
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if n == 0:
        return "IDENTICAL"
    return "diff=%d bbox=(%d,%d)-(%d,%d)" % (n, x0, y0, x1, y1)


def menu_present(a):
    w, h, d = a
    n = 0
    for y in range(68, 454, 2):
        for x in range(68, 252, 2):
            i = (y * w + x) * 3
            if d[i] + d[i + 1] + d[i + 2] < 250:
                n += 1
    return n


for pair in [(sys.argv[1], sys.argv[2])]:
    a, b = pair
    ia, ib = load(a), load(b)
    print("%s -> %s : %s" % (a, b, diff_bbox(ia, ib)))
    for f in pair:
        img = load(f)
        print("   %s menu-darkpx=%d" % (f, menu_present(img)))
