#!/usr/bin/env python3
import sys

def load_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        dims = f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        f.readline()
        d = f.read()
    return w, h, d

def bbox(a, b, thr=30):
    w, h, ad = a
    _, _, bd = b
    minx, miny, maxx, maxy = w, h, -1, -1
    n = 0
    for y in range(h):
        ro = y * w * 3
        for x in range(w):
            i = ro + x * 3
            dr = abs(ad[i] - bd[i]); dg = abs(ad[i+1]-bd[i+1]); db = abs(ad[i+2]-bd[i+2])
            if dr+dg+db > thr:
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    return n, (minx, miny, maxx, maxy)

if __name__ == "__main__":
    a = load_ppm(sys.argv[1])
    b = load_ppm(sys.argv[2])
    n, box = bbox(a, b)
    print("changed pixels: %d  bbox: %s" % (n, box))
