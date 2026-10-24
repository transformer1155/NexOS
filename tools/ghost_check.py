#!/usr/bin/env python3
def read_ppm(p):
    with open(p,'rb') as f:
        f.readline(); w,h=(int(x) for x in f.readline().split()); f.readline()
        return w,h,f.read()

base='_v_0_base.ppm'
w,h,a=read_ppm(base); _,_,b=read_ppm('_v_4_released.ppm')
_,_,c=read_ppm('_v_3_pressed.ppm')

# 1. 长按点 (400,500) 32x32 区域: released vs base — 幽灵会在这里
def boxdiff(A,B,x0,y0,sz=40):
    n=0
    for y in range(y0,min(y0+sz,h)):
        for x in range(x0,min(x0+sz,w)):
            i=(y*w+x)*3
            if A[i]!=B[i] or A[i+1]!=B[i+1] or A[i+2]!=B[i+2]: n+=1
    return n

print("长按点 (400,500) 40x40 after release:", boxdiff(a,b,390,490), "px differ")
print("  -> PASS (no ghost at press point)" if boxdiff(a,b,390,490)<50 else "  -> GHOST DETECTED")

# 2. 光标移动路径 (640,300)->(700,340)->(680,360)->(400,500): 每点查残影
pts=[(640,300),(700,340),(680,360)]
for (x,y) in pts:
    d=boxdiff(a,b,x-10,y-10,52)
    print(f"光标路径点 ({x},{y}): {d} px differ", "-> PASS" if d<50 else "-> GHOST")

# 3. 右上角差异是什么: 采样几个变化像素的 RGB
cnt=0
for y in range(0,60):
    for x in range(1140,1280):
        i=(y*w+x)*3
        if a[i]!=b[i] and cnt<5:
            print(f"  右上角 sample ({x},{y}): base=({a[i]},{a[i+1]},{a[i+2]}) rel=({b[i]},{b[i+1]},{b[i+2]})")
            cnt+=1
