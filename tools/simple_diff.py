#!/usr/bin/env python3
import sys

def main():
    with open('_t_base.ppm','rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        a = f.read()
    with open('_t_menu.ppm','rb') as f:
        f.readline()
        f.readline()
        f.readline()
        b = f.read()

    base_light = sum(1 for i in range(0, len(a), 3) if a[i]+a[i+1]+a[i+2] > 620)
    menu_light = sum(1 for i in range(0, len(b), 3) if b[i]+b[i+1]+b[i+2] > 620)
    
    diff = sum(1 for i in range(min(len(a),len(b))) if a[i] != b[i]) // 3  # pixels

    # 变化区域
    rows = []
    for y in range(h):
        c = 0
        for x in range(w):
            i = (y*w+x)*3
            if i+2 < len(a) and i+2 < len(b):
                if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]: c += 1
        if c > 5:
            rows.append((y, c))

    print(f"Frame: {w}x{h}")
    print(f"Base light: {base_light}")
    print(f"Menu light: {menu_light}")
    print(f"Light delta: {menu_light - base_light:+d}")
    print(f"Pixel diff: {diff}")
    print(f"Changed rows: {len(rows)}")
    if rows:
        fy, ly = rows[0][0], rows[-1][0]
        print(f"  Y range: {fy}-{ly}")
        for y, c in rows[:8]: print(f"    y={y}: {c}px")

main()
