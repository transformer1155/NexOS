#!/usr/bin/env python3
"""Minimal dependency-free PNG inspector.

Decodes a truecolour PNG (the format both QEMU screendumps-converted and
the WinForms host produce) and reports the dominant colours plus a few
named probe points, so a headless agent can verify a rendered frame
without an image viewer.

usage: png_probe.py <file.png> [x,y[:label] ...]
"""
import sys
import zlib
from collections import Counter


def decode(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    w = h = bitdepth = colour = None
    idat = bytearray()
    while pos < len(data):
        ln = int.from_bytes(data[pos:pos + 4], "big")
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w = int.from_bytes(body[0:4], "big")
            h = int.from_bytes(body[4:8], "big")
            bitdepth, colour = body[8], body[9]
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break

    assert bitdepth == 8, "only 8-bit PNGs supported"
    nch = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, nch, out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    w, h, nch, px = decode(path)
    print(f"{path}: {w}x{h} channels={nch}")

    def at(x, y):
        o = (y * w + x) * nch
        return px[o], px[o + 1], px[o + 2]

    c = Counter()
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            c[at(x, y)] += 1
    total = sum(c.values())
    print(f"  distinct(sampled)={len(c)}")
    for col, n in c.most_common(8):
        print(f"    #{col[0]:02X}{col[1]:02X}{col[2]:02X}  {100.0*n/total:5.1f}%")

    for spec in sys.argv[2:]:
        label = ""
        if ":" in spec:
            spec, label = spec.split(":", 1)
        x, y = (int(v) for v in spec.split(","))
        r, g, b = at(x, y)
        print(f"  ({x:4d},{y:4d}) #{r:02X}{g:02X}{b:02X}  {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
