#!/usr/bin/env python3
"""Minimal PPM (P6) -> PNG converter with optional downscaling.

No third-party dependencies: PNG is written directly with zlib.
Usage: ppm2png.py <in.ppm> <out.png> [max_width]
"""
import sys, zlib, struct


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # parse header tokens, skipping comments
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1  # single whitespace after maxval
    magic, w, h, _mx = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P6":
        raise ValueError("not a P6 PPM: %r" % magic)
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                          # filter type: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def downscale(w, h, rgb, factor):
    nw, nh = w // factor, h // factor
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        sy = y * factor
        for x in range(nw):
            sx = x * factor
            si = (sy * w + sx) * 3
            di = (y * nw + x) * 3
            out[di:di + 3] = rgb[si:si + 3]
    return nw, nh, bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    maxw = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    w, h, rgb = read_ppm(src)
    if maxw and w > maxw:
        factor = max(1, w // maxw)
        w, h, rgb = downscale(w, h, rgb, factor)
    write_png(dst, w, h, rgb)
    print("wrote %s (%dx%d)" % (dst, w, h))


if __name__ == "__main__":
    main()
