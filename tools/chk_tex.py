import struct, collections

def analyze(fn):
    d = open(fn, "rb").read()
    if d[:4] != b"TEX1":
        print(fn, "not TEX1"); return
    ver, w, h, fmt, nbytes = struct.unpack_from("<IIIII", d, 4)
    p = 24
    data = d[p:p + nbytes]
    cnt = collections.Counter()
    cnt_bgr = collections.Counter()
    step = max(1, len(data) // 20000)
    for i in range(0, len(data) - 3, step):
        b, g, r = data[i], data[i + 1], data[i + 2]
        cnt[(r, g, b)] += 1
        cnt_bgr[(b, g, r)] += 1
    print(fn, "ver", ver, "wxh", w, "x", h, "fmt", fmt, "bytes", nbytes)
    print("  as-RGB :", cnt.most_common(3))
    print("  as-BGR :", cnt_bgr.most_common(3))

for f in ["sfs_files/tex_wall.tex", "sfs_files/tex_k0.tex", "sfs_files/tex_task.tex"]:
    analyze(f)
