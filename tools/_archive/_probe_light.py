import sys
sys.path.insert(0, "tools")


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px


def count(path, y0=0, y1=None):
    w, h, px = read_ppm(path)
    if y1 is None:
        y1 = h
    n = 0
    total = 0
    for yy in range(y0, min(y1, h)):
        row = yy * w * 3
        for xx in range(0, w):
            i = row + xx * 3
            if px[i] + px[i + 1] + px[i + 2] > 700:
                n += 1
            total += 1
    return n, total


for name in ["build/rclick_base.ppm", "build/rclick_menu.ppm",
             "build/tb_pin_base.ppm", "build/tb_pin_menu.ppm"]:
    n_all, t_all = count(name)
    n_tb, t_tb = count(name, 672, 720)
    n_top, _ = count(name, 0, 600)
    print("%-28s all=%6d  taskbar(672+)=%5d  top600=%6d" % (name, n_all, n_tb, n_top))
