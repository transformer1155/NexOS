import sys, struct

def load_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(x) for x in f.readline().split()]
        maxv = int(f.readline())
        data = f.read()
    return w, h, data

def stats(path, label):
    w, h, d = load_ppm(path)
    n = w * h
    # sample every 7th pixel for speed
    colors = set()
    nonbg = 0
    # wallpaper base approx: sample top-right corner pixel as bg reference
    # count distinct quantized colors
    step = 3 * 7
    for i in range(0, len(d) - 3, step):
        r, g, b = d[i], d[i+1], d[i+2]
        colors.add((r >> 4, g >> 4, b >> 4))
    print(f"{label}: {w}x{h} colors(sampled,/16量化)={len(colors)}")
    return w, h, d, colors

if __name__ == "__main__":
    a = sys.argv[1]
    b = sys.argv[2] if len(sys.argv) > 2 else None
    w1, h1, d1, c1 = stats(a, "A="+a)
    if b:
        w2, h2, d2, c2 = stats(b, "B="+b)
        # pixel diff on full res (sampled)
        diff = 0
        tot = 0
        step = 3 * 3
        for i in range(0, min(len(d1), len(d2)) - 3, step):
            tot += 1
            if abs(d1[i]-d2[i]) > 24 or abs(d1[i+1]-d2[i+1]) > 24 or abs(d1[i+2]-d2[i+2]) > 24:
                diff += 1
        print(f"pixel diff: {diff}/{tot} = {100.0*diff/tot:.1f}%")
        print(f"distinct-color overlap: A={len(c1)} B={len(c2)} common={len(c1 & c2)}")
