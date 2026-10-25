#!/usr/bin/env python3
"""
ppm_to_png.py - Convert a QEMU PPM screendump to PNG (stdlib only, via zlib)
and report how much of the framebuffer is non-black (evidence of graphics).

Usage:
    python3 ppm_to_png.py <in.ppm> <out.png>
"""
import struct
import sys
import os
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6', "not a P6 PPM"
    # parse header: P6 <w> <h> <maxval>\n
    idx = 2
    fields = []
    while len(fields) < 3:
        # skip whitespace
        while idx < len(data) and data[idx] in b' \t\r\n':
            idx += 1
        end = idx
        while end < len(data) and data[end] not in b' \t\r\n':
            end += 1
        fields.append(int(data[idx:end]))
        idx = end
    w, h, maxval = fields
    # pixel data starts after the newline following maxval
    idx = data.index(b'\n', idx) + 1
    pixels = data[idx:idx + w * h * 3]
    return w, h, pixels


def write_png(path, w, h, pixels):
    def chunk(ctype, cdata):
        c = ctype + cdata
        return struct.pack('>I', len(cdata)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)  # filter type 0 (none) per scanline
        raw.extend(pixels[y * stride:(y + 1) * stride])
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', ihdr)
    out += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    out += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(out)


def analyze(pixels, w, h):
    non_black = 0
    total = w * h
    # sample stride to keep it fast on big images
    step = 3
    n = 0
    for i in range(0, len(pixels) - 2, step):
        if pixels[i] > 24 or pixels[i + 1] > 24 or pixels[i + 2] > 24:
            non_black += 1
        n += 1
    pct = 100.0 * non_black / max(n, 1)
    return non_black, n, pct


def main():
    if len(sys.argv) < 3:
        print("Usage: ppm_to_png.py <in.ppm> <out.png>")
        sys.exit(1)
    inp, outp = sys.argv[1], sys.argv[2]
    w, h, pixels = read_ppm(inp)
    write_png(outp, w, h, pixels)
    nb, n, pct = analyze(pixels, w, h)
    print(f"[PNG] {w}x{h} -> {outp}")
    print(f"[ANALYZE] non-black pixels: {nb}/{n} ({pct:.1f}%)")


if __name__ == '__main__':
    main()
