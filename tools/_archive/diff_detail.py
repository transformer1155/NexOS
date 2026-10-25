#!/usr/bin/env python3
import sys

p1, p2 = sys.argv[1], sys.argv[2]

def read_ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        return w, h, f.read()

w, h, a = read_ppm(p1)
_, _, b = read_ppm(p2)

# 统计每行变化
row_changes = []
for y in range(h):
    changed = 0
    for x in range(w):
        i = (y * w + x) * 3
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            changed += 1
    row_changes.append(changed)

# 找变化区域
rows_with_change = [(y, c) for y, c in enumerate(row_changes) if c > 10]
print(f"Changed rows (>10px changed): {len(rows_with_change)}")
if rows_with_change:
    first_y = rows_with_change[0][0]
    last_y = rows_with_change[-1][0]
    print(f"  Y range: {first_y} - {last_y} (height={last_y-first_y})")
    for y, c in rows_with_change[:5]:
        print(f"  y={y}: {c} changed px")
    if len(rows_with_change) > 5:
        print(f"  ... ({len(rows_with_change)-5} more rows)")

# 同样统计列
col_changes = [0] * w
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            col_changes[x] += 1

cols_with_change = [(x, c) for x, c in enumerate(col_changes) if c > 5]
if cols_with_change:
    first_x = cols_with_change[0][0]
    last_x = cols_with_change[-1][0]
    print(f"  X range: {first_x} - {last_x} (width={last_x-first_x})")
else:
    print("No columns with changes (only cursor move)")

# 具体区域的 RGB 值 (变化区域中心)
print(f"\nFrame: {w}x{h}")
total_changes = sum(1 for i in range(len(a)) if a[i] != b[i])
print(f"Total byte diff: {total_changes}")
print(f"As pixel diff: {total_changes // 3}")
