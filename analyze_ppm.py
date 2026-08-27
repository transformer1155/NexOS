import sys
f = r'D:\MyOS\bootloader\shot.ppm'
data = open(f, 'rb').read()
# PPM (P6) header: "P6\nW H\nMAX\n" then raw RGB
idx = 0
assert data[:2] == b'P6', data[:2]
# parse header
parts = []
i = 2
while len(parts) < 3:
    while data[i] in b' \t\n\r':
        i += 1
    s = i
    while data[i] not in b' \t\n\r':
        i += 1
    parts.append(int(data[s:i]))
    i += 1
w, h, mx = parts
print('size', w, h, 'max', mx)
px = data[i:]
# count non-black pixels and gather top colors
from collections import Counter
c = Counter()
nonblack = 0
total = w*h
step = 7  # sample every 7th pixel for speed
n = 0
for off in range(0, len(px)-3, 3*step):
    r, g, b = px[off], px[off+1], px[off+2]
    if r or g or b:
        nonblack += 1
    c[(r//16, g//16, b//16)] += 1
    n += 1
print('sampled', n, 'nonblack', nonblack, 'pct_nonblack=%.2f' % (100.0*nonblack/n))
print('top colors (r,g,b in /16):')
for col, cnt in c.most_common(8):
    print('  ', col, cnt)
