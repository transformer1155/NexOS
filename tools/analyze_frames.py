#!/usr/bin/env python3
import sys

def count_light(path, threshold=620):
    with open(path, 'rb') as f:
        hdr = f.readline().strip()
        w, h = (int(x) for x in f.readline().split())
        f.readline()  # maxval
        px = f.read()
    n = 0
    for i in range(0, len(px), 3):
        if px[i] + px[i+1] + px[i+2] > threshold:
            n += 1
    return n, w, h

def pixel_diff(p1, p2):
    with open(p1,'rb') as f: f.readline(); w,h = (int(x) for x in f.readline().split()); f.readline(); a=f.read()
    with open(p2,'rb') as f: f.readline(); f.readline(); f.readline(); b=f.read()
    diff = 0
    for i in range(len(a)):
        if a[i] != b[i]: diff += 1
    return diff, w, h

b, bw, bh = count_light('_qemu_base.ppm')
m, mw, mh = count_light('_qemu_menu.ppm')
pd, pw, ph = pixel_diff('_qemu_base.ppm', '_qemu_menu.ppm')

print(f"Frame size: {pw}x{ph}")
print(f"Base light: {b}")
print(f"Menu light: {m}")
print(f"Light delta: {m-b:+d}")
print(f"Pixel diff count: {pd}")
print(f"Pixel diff %: {pd/(pw*ph*3)*100:.2f}%")
