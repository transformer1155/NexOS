#!/usr/bin/env python3
import sys
p1, p2 = sys.argv[1], sys.argv[2]

def count_light(path, threshold=620):
    with open(path,'rb') as f:
        f.readline(); w,h=(int(x) for x in f.readline().split()); f.readline(); px=f.read()
    n=0
    for i in range(0, len(px), 3):
        if px[i]+px[i+1]+px[i+2]>threshold: n+=1
    return n, w, h

def diff_detail(p1, p2):
    with open(p1,'rb') as f: f.readline(); w,h=(int(x) for x in f.readline().split()); f.readline(); a=f.read()
    with open(p2,'rb') as f: f.readline(); f.readline(); f.readline(); b=f.read()
    row_changes=[]
    for y in range(h):
        c=0
        for x in range(w):
            i=(y*w+x)*3
            if a[i]!=b[i] or a[i+1]!=b[i+1] or a[i+2]!=b[i+2]: c+=1
        if c>5: row_changes.append((y,c))
    return row_changes, w, h

b, bw, bh = count_light(p1)
m, mw, mh = count_light(p2)
rows, w, h = diff_detail(p1, p2)

print(f"Frame: {w}x{h}")
print(f"Base light: {b}")
print(f"Menu light: {m}")
print(f"Light delta: {m-b:+d}")
print(f"Rows changed (>5px): {len(rows)}")
if rows:
    fy = rows[0][0]; ly = rows[-1][0]
    print(f"  Y range: {fy}-{ly}")
    for y,c in rows[:10]: print(f"    y={y}: {c}px")

total_diff = sum(1 for i in range(w*h*3) if open(p1,'rb').read()[-w*h*3:][i] != open(p2,'rb').read()[-w*h*3:][i])
print(f"Total pixel diff: {total_diff}")
