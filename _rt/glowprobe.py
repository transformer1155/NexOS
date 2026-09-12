import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/splash_t03.ppm"
with open(path, "rb") as f:
    data = f.read()

# P6 header: magic, w, h, maxval
parts = data.split(b"\n", 3)
assert parts[0].strip() == b"P6", parts[0]
i = 0
toks = []
pos = 2
while len(toks) < 3:
    while data[pos:pos+1].isspace():
        pos += 1
    if data[pos:pos+1] == b"#":
        while data[pos:pos+1] not in (b"\n", b""):
            pos += 1
        continue
    s = pos
    while not data[pos:pos+1].isspace():
        pos += 1
    toks.append(int(data[s:pos]))
pos += 1
w, h, maxv = toks
px = data[pos:]

def pix(x, y):
    o = (y * w + x) * 3
    return px[o], px[o+1], px[o+2]

# glow centre row: logo_y = h*38/100, scale = w/280 clamp[2,6], glow_cy = logo_y+8*scale
scale = max(2, min(6, w // 280))
logo_y = h * 38 // 100
gy = logo_y + 8 * scale
gx = w // 2
rx = w // 3
print("w=%d h=%d scale=%d gy=%d gx=%d rx=%d" % (w, h, scale, gy, gx, rx))
print("row %d, R value sampled across the glow:" % gy)
row = []
for frac in (0, 20, 40, 60, 80, 90, 95, 99, 100):
    x = gx + rx * frac // 100
    x = min(x, w - 1)
    r, g, b = pix(x, gy)
    row.append("x+%d%%:(%d,%d,%d)" % (frac, r, g, b))
print("  " + "  ".join(row))
# vertical profile
print("column %d, R sampled down the glow:" % gx)
col = []
for frac in (0, 25, 50, 75, 100):
    y = gy - 110 + 220 * frac // 100
    y = max(0, min(y, h - 1))
    r, g, b = pix(gx + rx // 2, y)
    col.append("dy%d:(%d,%d,%d)" % (frac - 50, r, g, b))
print("  " + "  ".join(col))
