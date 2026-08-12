#!/usr/bin/env python3
import sys, zlib, struct

def ppm_to_png(src, dst):
    with open(src, "rb") as f:
        assert f.readline().strip() == b"P6"
        dims = f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        f.readline()
        raw = f.read()
    # build filtered scanlines (filter byte 0)
    stride = w * 3
    out = bytearray()
    for y in range(h):
        out.append(0)
        out += raw[y*stride:(y+1)*stride]
    comp = zlib.compress(bytes(out), 9)
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        c += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        return c
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)
    print("wrote", dst, w, h)

if __name__ == "__main__":
    ppm_to_png(sys.argv[1], sys.argv[2])
