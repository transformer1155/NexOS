#!/usr/bin/env python3
"""Minimal PPM (P6) -> PNG converter using only the stdlib (zlib)."""
import sys, zlib, struct


def ppm_to_png(src, dst):
    with open(src, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()  # maxval
        data = f.read()
    if len(data) < w * h * 3:          # header lied; derive height from bytes
        h = len(data) // (w * 3)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 (None)
        raw.extend(data[y * w * 3:(y + 1) * w * 3])
    comp = zlib.compress(bytes(raw), 9)

    def chunk(typ, body):
        c = struct.pack(">I", len(body)) + typ + body
        c += struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)
        return c

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)
    print(f"wrote {dst} ({w}x{h}, {len(png)} bytes)")


if __name__ == "__main__":
    ppm_to_png(sys.argv[1], sys.argv[2])
