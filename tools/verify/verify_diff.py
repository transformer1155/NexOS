#!/usr/bin/env python3
import sys

def read_ppm(p):
    with open(p,'rb') as f:
        f.readline(); w,h=(int(x) for x in f.readline().split()); f.readline()
        return w,h,f.read()

def light(px, t=620):
    return sum(1 for i in range(0,len(px),3) if px[i]+px[i+1]+px[i+2]>t)

def diffbox(p1,p2):
    w,h,a=read_ppm(p1); _,_,b=read_ppm(p2)
    n=0; minx=w; miny=h; maxx=0; maxy=0
    for y in range(h):
        for x in range(w):
            i=(y*w+x)*3
            if a[i]!=b[i] or a[i+1]!=b[i+1] or a[i+2]!=b[i+2]:
                n+=1
                if x<minx: minx=x
                if x>maxx: maxx=x
                if y<miny: miny=y
                if y>maxy: maxy=y
    return n,(minx,miny,maxx,maxy),w,h

base='_v_0_base.ppm'
# S1: 菜单必须出现 (大量新像素)
n,box,w,h = diffbox(base,'_v_1_menu_frozen.ppm')
print(f"S1 menu-vs-base: {n} px diff, box={box}")
print(f"   PASS: menu rendered while frozen" if n>15000 else "   FAIL: menu missing/incomplete")
# S2: hover 后菜单还在且更大变化
n2,box2,_,_ = diffbox('_v_1_menu_frozen.ppm','_v_2_hover.ppm')
print(f"S2 hover-vs-menu: {n2} px diff, box={box2}")
# S3: 长按释放后 vs base: 不应有大块残影 (重影=光标旧位置 32x32 持续差异)
n3,box3,_,_ = diffbox(base,'_v_4_released.ppm')
print(f"S3 released-vs-base: {n3} px diff, box={box3}")
if n3 < 3000:
    print("   PASS: no ghost after long-press")
else:
    print(f"   CHECK: {n3} px differ (may be legit UI state, inspect box {box3})");
