#!/usr/bin/env python3
with open('_t_base.ppm','rb') as f:
    f.readline(); w, h = (int(x) for x in f.readline().split()); f.readline(); a = f.read()
with open('_t_menu.ppm','rb') as f:
    f.readline(); f.readline(); f.readline(); b = f.read()

# 采样 Y=50-200 变化区域的 RGB
print("=== Sample changed regions ===")
for y in range(30, 350, 30):
    changes = []
    for x in range(0, w, 5):
        i = (y*w+x)*3
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            changes.append((x, y, a[i], a[i+1], a[i+2], b[i], b[i+1], b[i+2]))
            if len(changes) >= 3: break
    if changes:
        print(f"y={y}:")
        for c in changes:
            print(f"  x={c[0]} old=({c[2]},{c[3]},{c[4]}) new=({c[5]},{c[6]},{c[7]})")

# 看 base vs menu 的整体平均亮度
base_sum = sum(a[i]+a[i+1]+a[i+2] for i in range(0, len(a), 3))
menu_sum = sum(b[i]+b[i+1]+b[i+2] for i in range(0, len(b), 3))
print(f"\nBase avg brightness: {base_sum/(len(a)//3):.1f}")
print(f"Menu avg brightness: {menu_sum/(len(b)//3):.1f}")
print(f"Delta: {menu_sum - base_sum:+d}")

# 看 500-650 列 (屏幕中部) 的差异区域
print("\n=== Center strip (y=200-400, x=400-900) ===")
for y in range(200, 400, 20):
    row_changes = 0
    for x in range(400, 900):
        i = (y*w+x)*3
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]: row_changes += 1
    if row_changes > 0: print(f"  y={y}: {row_changes} changed px in center strip")
