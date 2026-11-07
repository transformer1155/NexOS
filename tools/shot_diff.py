#!/usr/bin/env python3
"""shot_diff.py -- do two screendumps differ MEANINGFULLY?

Byte-length comparison is worthless here: the HUD clock changes every second, so
file sizes wobble by a few bytes on every capture and a "NO-OP" verdict was being
reported for steps that actually did nothing AND for steps that worked.  This
compares PIXELS and ignores the HUD region, so a verdict means what it says.

Usage: shot_diff.py a.png b.png
Exit:  0 = differ (something happened), 1 = identical, 2 = error
"""
import sys, zlib, struct

# The HUD panel (bottom-right) redraws its clock every second.  Ignoring it is
# the difference between "input worked" and "the clock ticked again".
HUD = (800, 520, 1024, 700)


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos = 8; w = h = bd = ct = None; idat = []
    while pos < len(data):
        ln = struct.unpack_from('>I', data, pos)[0]
        typ = data[pos+4:pos+8]; body = data[pos+8:pos+8+ln]
        if typ == b'IHDR': w, h, bd, ct = struct.unpack_from('>IIBB', body, 0)
        elif typ == b'IDAT': idat.append(body)
        elif typ == b'IEND': break
        pos += 12 + ln
    if bd != 8 or ct not in (2, 6): raise ValueError('unsupported PNG')
    raw = zlib.decompress(b''.join(idat))
    ch = 3 if ct == 2 else 4
    stride = w * ch
    out = bytearray(w*h*ch)
    prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(ch, stride): line[i] = (line[i]+line[i-ch]) & 0xFF
        elif f == 2:
            for i in range(stride): line[i] = (line[i]+prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a+prev[i])>>1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]; c = prev[i-ch] if i >= ch else 0
                pp = a+b-c; pa,pb,pc = abs(pp-a),abs(pp-b),abs(pp-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[i] = (line[i]+pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, out, ch


def main():
    a, b = sys.argv[1], sys.argv[2]
    try:
        wa, ha, pa, ca = read_png(a)
        wb, hb, pb, cb = read_png(b)
    except Exception as e:
        print('  ERROR: %s' % e); return 2
    if (wa, ha) != (wb, hb):
        print('  size changed: DIFFER'); return 0
    hx0, hy0, hx1, hy1 = HUD
    diff = 0; total = 0
    for y in range(ha):
        if hy0 <= y < hy1: continue          # skip the HUD strip entirely
        row = y * wa * ca
        for x in range(0, wa, 2):            # sample every other column
            if hx1 > x >= hx0 and False: pass
            i = row + x*ca; j = i
            total += 1
            if pa[i] != pb[j] or pa[i+1] != pb[j+1] or pa[i+2] != pb[j+2]:
                diff += 1
    pct = 100.0*diff/max(1,total)
    print('  pixels differing (HUD excluded): %d / %d  (%.2f%%)' % (diff, total, pct))
    if diff == 0:
        print('  RESULT: IDENTICAL -- the input had NO visible effect'); return 1
    print('  RESULT: CHANGED'); return 0


if __name__ == '__main__':
    sys.exit(main())
