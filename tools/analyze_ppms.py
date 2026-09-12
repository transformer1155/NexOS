#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线分析已抓取的 PPM: 定位右键菜单(净变化像素)并看 poll_0 是否异分辨率/黑。"""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"P6":
        return None
    i = 2; nums = []
    while len(nums) < 3:
        while i < len(d) and d[i] in b" \t\r\n": i += 1
        s = i
        while i < len(d) and d[i] in b"0123456789": i += 1
        nums.append(int(d[s:i])); i += 1
    while i < len(d) and d[i] in b" \t\r\n": i += 1
    w, h, maxval = nums
    px = d[i:i + w * h * 3]
    if len(px) < w * h * 3: return None
    return w, h, px


def nonblack_bbox(img, thr=40):
    w, h, px = img
    minx, miny, maxx, maxy = w, h, -1, -1; cnt = 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            if px[o] + px[o+1] + px[o+2] > thr:
                cnt += 1
                minx = min(minx, x); miny = min(miny, y)
                maxx = max(maxx, x); maxy = max(maxy, y)
    return cnt, (minx, miny, maxx, maxy)


def changed_bbox(before, after, dthr=60):
    w, h, pb = before
    _, _, pa = after
    minx, miny, maxx, maxy = w, h, -1, -1; cnt = 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            if abs((pa[o]+pa[o+1]+pa[o+2]) - (pb[o]+pb[o+1]+pb[o+2])) > dthr:
                cnt += 1
                minx = min(minx, x); miny = min(miny, y)
                maxx = max(maxx, x); maxy = max(maxy, y)
    return cnt, (minx, miny, maxx, maxy)


def main():
    L = read_ppm("build/verify_gui_poll_1.ppm")
    print("poll_1 (desktop) .ppm: %dx%d" % (L[0], L[1]))
    cnt, box = nonblack_bbox(L)
    print("  非黑占比 %.2f%%  黑边 上=%d 下=%d 左=%d 右=%d" % (
        100.0*cnt/(L[0]*L[1]), box[1], L[1]-1-box[3], box[0], L[0]-1-box[2]))

    # poll_0 早期帧
    p0 = read_ppm("build/verify_gui_poll_0.ppm")
    if p0:
        print("poll_0.ppm: %dx%d  size=%d" % (p0[0], p0[1], os.path.getsize("build/verify_gui_poll_0.ppm")))
        c0, b0 = nonblack_bbox(p0)
        print("  非黑占比 %.2f%%  黑边 上=%d 下=%d 左=%d 右=%d" % (
            100.0*c0/(p0[0]*p0[1]), b0[1], p0[1]-1-b0[3], b0[0], p0[0]-1-b0[2]))

    for tag in ("rclick_mid", "rclick_tl"):
        R = read_ppm("build/verify_gui_%s.ppm" % tag)
        if not R:
            print(tag, "缺失"); continue
        c, b = changed_bbox(L, R)
        cxm = (b[0]+b[2])//2; cym = (b[1]+b[3])//2
        print("%s: 净变化像素=%d  变化区 x[%d..%d] y[%d..%d]  中心(%d,%d)  菜单尺寸 %dx%d" % (
            tag, c, b[0], b[2], b[1], b[3], cxm, cym, b[2]-b[0]+1, b[3]-b[1]+1))


if __name__ == "__main__":
    main()
