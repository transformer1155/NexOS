import sys, struct, zlib
def read_ppm(path):
    data = open(path, 'rb').read()
    assert data[:2] == b'P6', data[:2]
    i = 2; parts = []
    while len(parts) < 3:
        while data[i] in b' \t\n\r': i += 1
        s = i
        while data[i] not in b' \t\n\r': i += 1
        parts.append(int(data[s:i])); i += 1
    w, h, mx = parts
    return w, h, data[i:]
def save_png(path, w, h, rgb):
    def chunk(typ, body):
        c = typ + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw.extend(rgb[y*w*3:(y+1)*w*3])
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 6)
    open(path, 'wb').write(sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b''))
if __name__ == '__main__':
    w, h, px = read_ppm(sys.argv[1]); save_png(sys.argv[2], w, h, px); print('ok', w, h)
