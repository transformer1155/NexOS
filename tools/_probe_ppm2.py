#!/usr/bin/env python3
"""Inspect the PPM pairs from the failed rclick run."""
import os, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

def read_ppm(path):
    with open(path, "rb") as f:
        hdr = f.readline().strip()
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return hdr, w, h, px

pairs = [
    ("rclick_base", "rclick_menu"),
    ("tb_pin_base", "tb_pin_menu"),
    ("tb_bar_base", "tb_bar_menu"),
    ("tray_base", "tray_menu"),
    ("icon_base", "icon_menu"),
    ("rclick_win_base", "rclick_win_menu"),
]
for a, b in pairs:
    pa, pb = os.path.join("build", a + ".ppm"), os.path.join("build", b + ".ppm")
    ea = os.path.exists(pa)
    eb = os.path.exists(pb)
    if not (ea and eb):
        print("%s vs %s  MISSING  (a=%s b=%s)" % (a, b, ea, eb))
        continue
    ha, wa, ha_, pxa = read_ppm(pa)
    hb, wb, hb_, pxb = read_ppm(pb)
    same = pxa == pxb
    md5a = hashlib.md5(pxa).hexdigest()[:8]
    md5b = hashlib.md5(pxb).hexdigest()[:8]
    print("%s vs %s  sizes %dx%d/%dx%d same=%s md5=%s/%s len=%d/%d" % (
        a, b, wa, ha_, wb, hb_, same, md5a, md5b, len(pxa), len(pxb)))
