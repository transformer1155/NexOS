import sys, struct, zlib

def read_ppm(path):
    data = open(path, 'rb').read()
    assert data[:2] == b'P6', data[:2]
    i = 2
    parts = []
    while len(parts) < 3:
        while data[i] in b' \t\n\r':
            i += 1
        s = i
        while data[i] not in b' \t\n\r':
            i += 1
        parts.append(int(data[s:i]))
        i += 1
    w, h, mx = parts
    return w, h, data[i:]

def save_png(path, w, h, rgb):
    # rgb: bytes length w*h*3
    def chunk(typ, body):
        c = typ + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y*w*3:(y+1)*w*3])
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 6)
    out = sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')
    open(path, 'wb').write(out)

def downscale(w, h, px, nw, nh):
    # px: bytes w*h*3
    out = bytearray(nw*nh*3)
    for y in range(nh):
        for x in range(nw):
            sx = x*w//nw
            sy = y*h//nh
            o = (sy*w+sx)*3
            o2 = (y*nw+x)*3
            out[o2]=px[o]; out[o2+1]=px[o+1]; out[o2+2]=px[o+2]
    return bytes(out)

def main():
    w, h, px = read_ppm(r'D:\MyOS\bootloader\shot.ppm')
    # try PIL for nice downscale
    try:
        from PIL import Image
        im = Image.frombytes('RGB', (w,h), px)
        im = im.resize((320,180))
        im.save(r'D:\MyOS\bootloader\shot_small.png')
        print('saved via PIL', 320, 180)
    except Exception as e:
        small = downscale(w,h,px,320,180)
        save_png(r'D:\MyOS\bootloader\shot_small.png', 320,180, small)
        print('saved via manual png', 320, 180, '| PIL err:', e)

main()
