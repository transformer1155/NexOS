import sys
fn = sys.argv[1]
with open(fn, "rb") as f:
    assert f.readline().strip() == b"P6"
    w, h = map(int, f.readline().split())
    f.readline()
    d = f.read()
x0, x1, y0, y1 = 380, 900, 232, 640
dark = 0; tot = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        o = (y * w + x) * 3
        r, g, b = d[o], d[o+1], d[o+2]
        tot += 1
        if r < 60 and g < 60 and b < 70:
            dark += 1
print("%s dark_px=%d tot=%d frac=%.3f" % (fn, dark, tot, dark/tot))
