#!/usr/bin/env python3
import sys, zlib, struct

def read_ppm(path):
    d = open(path, 'rb').read()
    # parse P6 header (magic, w, h, maxval) skipping comments/whitespace
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(d) and d[i:i+1].isspace():
            i += 1
        if d[i:i+1] == b'#':
            while i < len(d) and d[i:i+1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace():
            j += 1
        parts.append(d[i:j]); i = j
    i += 1  # single whitespace after maxval
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w*h*3]

def write_png(path, w, h, rgb):
    raw = bytearray()
    row = w*3
    for y in range(h):
        raw.append(0)
        raw += rgb[y*row:(y+1)*row]
    def chunk(typ, data):
        c = struct.pack('>I', len(data)) + typ + data
        return c + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

for p in sys.argv[1:]:
    w, h, rgb = read_ppm(p)
    out = p.rsplit('.', 1)[0] + '.png'
    write_png(out, w, h, rgb)
    print("wrote", out, w, h)
