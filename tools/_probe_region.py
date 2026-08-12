import sys
sys.path.insert(0, "tools")

def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()  # maxval
        px = f.read()
    return w, h, px

def count_light_region(path, x0, y0, rw, rh):
    w, h, px = read_ppm(path)
    print("dims", w, h, "px len", len(px), "expect", w * h * 3)
    x0 = max(0, min(x0, w - 1))
    y0 = max(0, min(y0, h - 1))
    x1 = min(w, x0 + rw)
    y1 = min(h, y0 + rh)
    n = 0
    for yy in range(y0, y1):
        row = yy * w * 3
        for xx in range(x0, x1):
            i = row + xx * 3
            if i + 2 >= len(px):
                print("OOB at", yy, xx, i, "len", len(px))
                return -1
            if px[i] + px[i + 1] + px[i + 2] > 700:
                n += 1
    return n

print("base  ->", count_light_region("build/tb_pin_base.ppm", 485, 618, 280, 140))
print("menu  ->", count_light_region("build/tb_pin_menu.ppm", 485, 618, 280, 140))
print("rclick_base ->", count_light_region("build/rclick_base.ppm", 0, 46, 330, 400))
