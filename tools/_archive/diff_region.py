#!/usr/bin/env python3
import sys

with open('_t_base.ppm','rb') as f:
    f.readline(); w, h = (int(x) for x in f.readline().split()); f.readline(); a = f.read()
with open('_t_menu.ppm','rb') as f:
    f.readline(); f.readline(); f.readline(); b = f.read()

# 右键区域: x=620-660, y=280-320
print("=== Right-click region diff ===")
diff_count = 0
for y in range(280, 320):
    for x in range(620, 660):
        i = (y*w + x) * 3
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            if diff_count < 20:
                print(f"  ({x},{y}): ({a[i]},{a[i+1]},{a[i+2]}) -> ({b[i]},{b[i+1]},{b[i+2]})")
            diff_count += 1

print(f"  Total diff px in this region: {diff_count}")
print()

# 同时看屏幕右上角 (y=10-50) - boot splash/UI 残留?
print("=== Top strip (y=10-50) diff ===")
top_diff = 0
for y in range(10, 50):
    row_diff = sum(1 for x in range(w) if a[(y*w+x)*3:(y*w+x)*3+3] != b[(y*w+x)*3:(y*w+x)*3+3])
    if row_diff > 0:
        top_diff += row_diff
        print(f"  y={y}: {row_diff} px changed")
print(f"  Top strip total: {top_diff}")
