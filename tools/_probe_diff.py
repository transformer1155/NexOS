import sys
sys.path.insert(0, "tools")


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px


def region_diff(a, b, x0, y0, rw, rh, out):
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    w = min(wa, wb)
    h = min(ha, hb)
    with open(out, "wb") as f:
        f.write(("P6\n%d %d\n255\n" % (rw, rh)).encode())
        for yy in range(y0, y0 + rh):
            for xx in range(x0, x0 + rw):
                if 0 <= yy < h and 0 <= xx < w:
                    i = yy * w * 3 + xx * 3
                    da = abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + abs(pa[i + 2] - pb[i + 2])
                    if da > 30:
                        f.write(bytes(pa[i:i + 3]))
                    else:
                        f.write(b"\x00\x00\x00")
                else:
                    f.write(b"\x00\x00\x00")
    print("wrote", out)


# 1) whole-screen diff of the pin test
region_diff("build/tb_pin_base.ppm", "build/tb_pin_menu.ppm", 0, 0, 1280, 720,
            "build/_diff_pin.ppm")

# 2) whole-screen diff of the desktop-menu test (phase 1)
region_diff("build/rclick_base.ppm", "build/rclick_menu.ppm", 0, 0, 1280, 720,
            "build/_diff_desk.ppm")

# 3) dump the light counts for a few rows around the taskbar menu
_, _, pb = read_ppm("build/tb_pin_menu.ppm")
_, _, pa = read_ppm("build/tb_pin_base.ppm")
for yy in range(600, 720, 10):
    lightb = sum(1 for xx in range(480, 700, 3)
                 if pa[yy * 1280 * 3 + xx * 3] + pa[yy * 1280 * 3 + xx * 3 + 1] + pa[yy * 1280 * 3 + xx * 3 + 2] > 700)
    lightm = sum(1 for xx in range(480, 700, 3)
                 if pb[yy * 1280 * 3 + xx * 3] + pb[yy * 1280 * 3 + xx * 3 + 1] + pb[yy * 1280 * 3 + xx * 3 + 2] > 700)
    print("row", yy, "base", lightb, "menu", lightm)
