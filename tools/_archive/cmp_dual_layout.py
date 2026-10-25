def load_ppm(path):
    f = open(path, "rb")
    assert f.readline().strip() == b"P6"
    dims = f.readline().split()
    w, h = int(dims[0]), int(dims[1])
    f.readline()
    d = f.read()
    f.close()
    return d, w, h

def analyze(path, name):
    px, w, h = load_ppm(path)
    # 1) taskbar band: y in [h-48, h-8) - find non-background "elements"
    # 2) icon area: x in [0, 400), y in [60, 600)
    def band_edges(y0, y1, x0, x1):
        # vertical edges (x positions where content changes) via column diff
        cols = []
        prev = None
        for x in range(x0, x1, 2):
            r = g = b = n = 0
            for y in range(y0, y1, 2):
                i = (y * w + x) * 3
                r += px[i]; g += px[i+1]; b += px[i+2]; n += 1
            avg = (r//n, g//n, b//n)
            cols.append(avg)
        edges = 0
        for i in range(1, len(cols)):
            a, b = cols[i-1], cols[i]
            if abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2]) > 40:
                edges += 1
        return edges

    tb = band_edges(h-48, h-8, 0, w)
    print("%s taskbar columns w/ strong edges: %d" % (name, tb))

    # icon grid: count bright/dark blobs in left region (non-wallpaper pixels)
    n_blob = 0
    for y in range(60, 560, 4):
        for x in range(0, 500, 4):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i+1], px[i+2]
            # icon-ish: high contrast vs deep blue gradient
            if (r > 150 and g > 150 and b > 150) or (r < 40 and g < 40 and b < 60 and g > 10):
                pass
    # simpler: count strongly-lit pixels in left region
    lit = 0
    for y in range(60, 600, 3):
        for x in range(0, 500, 3):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i+1], px[i+2]
            if r + g + b > 480:
                lit += 1
    print("%s bright px in icon area: %d" % (name, lit))

analyze("build/dual_32.ppm", "k32")
analyze("build/dual_64.ppm", "k64")
