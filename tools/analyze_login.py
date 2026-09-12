#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""捕获登录/锁屏帧, 用"右键前后新增亮像素"定位右键菜单位置, 并量底部黑条。"""
import os, sys, time, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_gui as V
ROOT = V.ROOT
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


def nonblack_bbox(img, lum_thresh=40):
    w, h, px = img
    minx, miny, maxx, maxy = w, h, -1, -1; cnt = 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            if px[o] + px[o + 1] + px[o + 2] > lum_thresh:
                cnt += 1
                minx = min(minx, x); miny = min(miny, y)
                maxx = max(maxx, x); maxy = max(maxy, y)
    return cnt, (minx, miny, maxx, maxy)


def added_bright_bbox(before, after, th_lo=300, th_hi=520):
    """after 相对 before 净变化(双向)的像素包围盒 -> 定位右键菜单。"""
    return changed_bbox(before, after, 60)


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


def capture(q, tag):
    ppm = "build/verify_gui_%s.ppm" % tag
    q.cmd("screendump %s" % ppm, read=False)
    time.sleep(0.5)
    return read_ppm(ppm)


def main():
    q = V.QEMU("build/os_v2.img", headed=False, timeout=180)
    q.launch()
    L = None; t = 0
    for _ in range(40):
        im = capture(q, "poll_%d" % t); t += 1
        if im:
            cnt, box = nonblack_bbox(im)
            if cnt > im[0]*im[1]*0.30:
                L = im; break
        time.sleep(1)
    if L is None:
        print("未捕获到 GUI 帧"); q.quit(); return
    w, h, _ = L
    cnt, box = nonblack_bbox(L)
    print("=== 登录/锁屏帧 %dx%d ===" % (w, h))
    print("非黑占比 %.2f%%  黑边: 上=%d 下=%d 左=%d 右=%d" % (
        100.0*cnt/(w*h), box[1], h-1-box[3], box[0], w-1-box[2]))
    bc = sum(1 for y in range(h-60, h) for x in range(w)
             if L[2][(y*w+x)*3]+L[2][(y*w+x)*3+1]+L[2][(y*w+x)*3+2] > 1)
    print("底部60行非(近)黑像素=%d / %d %s" % (bc, w*60, "(有黑条)" if bc < w*60*0.9 else "(无黑条)"))

    # 实验1: 中点(640,360)右键 -> 菜单应在光标附近
    q.right_click(w//2, h//2); time.sleep(0.5)
    r1 = capture(q, "rclick_mid")
    if r1:
        c1, b1 = added_bright_bbox(L, r1)
        print("--- 中点右键: 净变化像素=%d  变化区 x[%d..%d] y[%d..%d] 中心(%d,%d)  光标(640,360)" % (
            c1, b1[0], b1[2], b1[1], b1[3], (b1[0]+b1[2])//2, (b1[1]+b1[3])//2))
        near = (abs((b1[0]+b1[2])//2 - w//2) < 140 and abs((b1[1]+b1[3])//2 - h//2) < 140)
        print("    -> 菜单%s光标处" % ("跟随" if near else "未跟随(异常! 落在 %d,%d 附近)" % ((b1[0]+b1[2])//2,(b1[1]+b1[3])//2)))
    q.click(20, h-20); time.sleep(0.4)

    # 实验2: 左上角(10,10)右键 -> 菜单应在左上角
    q.right_click(10, 10); time.sleep(0.5)
    r2 = capture(q, "rclick_tl")
    if r2:
        c2, b2 = added_bright_bbox(L, r2)
        print("--- 左上角(10,10)右键: 净变化像素=%d  变化区 x[%d..%d] y[%d..%d] 中心(%d,%d)" % (
            c2, b2[0], b2[2], b2[1], b2[3], (b2[0]+b2[2])//2, (b2[1]+b2[3])//2))
    q.quit()


if __name__ == "__main__":
    main()
