#!/usr/bin/env python3
"""shot_check.py -- decide whether a VirtualBox screenshot is effectively BLANK.

This exists because "does the screen actually show the desktop?" was repeatedly
answered by a human looking at the VM window.  That is a terrible feedback loop:
every display change cost a round trip, and a black screen could sit unnoticed
for several minutes.  A screendump is a file, so the check belongs in the build
loop.

Distinct-colour count is the right metric here: a working desktop has a
gradient wallpaper, 15 icons, a taskbar and a HUD -- thousands of distinct
colours.  A black (or otherwise failed) screen collapses to a handful.

Usage:  shot_check.py <png> [min_distinct]
Exit:   0 = looks like a real desktop, 1 = blank/black, 2 = cannot read
"""
import sys, zlib, struct


def read_png(path):
    """Minimal PNG reader: 8-bit RGB/RGBA, non-interlaced. Returns (w,h,pixels)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos = 8
    w = h = bitdepth = colortype = None
    idat = []
    while pos < len(data):
        ln = struct.unpack_from('>I', data, pos)[0]
        typ = data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, bitdepth, colortype = struct.unpack_from('>IIBB', body, 0)
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + ln
    if bitdepth != 8 or colortype not in (2, 6):
        raise ValueError('unsupported PNG (bitdepth=%s colortype=%s)' % (bitdepth, colortype))
    raw = zlib.decompress(b''.join(idat))
    channels = 3 if colortype == 2 else 4
    stride = w * channels
    out = bytearray(w * h * channels)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(channels, stride): line[i] = (line[i] + line[i-channels]) & 0xFF
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i-channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i-channels] if i >= channels else 0
                b = prev[i]
                c = prev[i-channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, out, channels


def main():
    path = sys.argv[1]
    need = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    try:
        w, h, px, ch = read_png(path)
    except Exception as e:
        print('  RESULT: UNREADABLE (%s)' % e)
        return 2
    colours = set()
    nonblack = 0
    step = max(1, (w*h) // 200000)          # sample at most ~200k pixels
    for i in range(0, w*h, step):
        o = i*ch
        r, g, b = px[o], px[o+1], px[o+2]
        if r > 8 or g > 8 or b > 8:
            nonblack += 1
        colours.add((r >> 2, g >> 2, b >> 2))    # quantise: ignore dithering
        if len(colours) > need and nonblack > (w*h//step)//20:
            break
    sampled = (w*h + step - 1)//step
    pct = 100.0 * nonblack / max(1, sampled)
    print('  image        : %dx%d' % (w, h))
    print('  sampled px   : %d' % sampled)
    print('  non-black    : %.1f%%' % pct)
    print('  distinct hue : %d (need >= %d)' % (len(colours), need))
    ok = len(colours) >= need and pct >= 5.0
    print('  RESULT: %s' % ('DESKTOP VISIBLE' if ok else 'BLANK / BLACK SCREEN'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
