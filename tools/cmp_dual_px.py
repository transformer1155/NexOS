import collections

def load_ppm(path):
    f = open(path, "rb")
    assert f.readline().strip() == b"P6"
    dims = f.readline().split()
    w, h = int(dims[0]), int(dims[1])
    f.readline()  # maxval
    d = f.read()
    f.close()
    return d, w, h

for f in ["build/dual_32.ppm", "build/dual_64.ppm"]:
    px, w, h = load_ppm(f)
    print("==", f, w, h)

    def pxat(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]

    def region_avg(x0, y0, x1, y1):
        r = g = b = n = 0
        for y in range(y0, y1, 8):
            for x in range(x0, x1, 8):
                rr, gg, bb = pxat(x, y)
                r += rr; g += gg; b += bb; n += 1
        return (r // n, g // n, b // n)

    print("  center   :", region_avg(int(w * 0.25), int(h * 0.2), int(w * 0.75), int(h * 0.6)))
    print("  left-mid :", region_avg(int(w * 0.05), int(h * 0.2), int(w * 0.2), int(h * 0.6)))
    print("  taskbar  :", region_avg(int(w * 0.05), int(h * 0.93), int(w * 0.95), int(h * 0.98)))
    cnt = collections.Counter()
    for y in range(0, h, 16):
        for x in range(0, w, 16):
            cnt[pxat(x, y)] += 1
    print("  top colors:", cnt.most_common(5))
