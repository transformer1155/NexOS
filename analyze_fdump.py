import sys
f = r'D:\MyOS\bootloader\fdump.bin'
data = open(f, 'rb').read()
print('size', len(data))
# treat as 32-bit BGRX (qemu boots VGA is BGRX? kernel says BGRX32)
# sample
from collections import Counter
c = Counter()
nonblack = 0
n = 0
for off in range(0, len(data)-4, 4*7):
    px = int.from_bytes(data[off:off+4], 'little')  # BGRX little-endian
    b = px & 0xFF
    g = (px>>8)&0xFF
    r = (px>>16)&0xFF
    if r or g or b:
        nonblack += 1
    c[(r//16,g//16,b//16)] += 1
    n += 1
print('sampled', n, 'nonblack', nonblack, 'pct=%.2f'%(100.0*nonblack/n))
for col,cnt in c.most_common(10):
    print('  ', col, cnt)
# also report first 200 pixels raw to see clear color
print('first 8 px (r,g,b):')
for i in range(8):
    px = int.from_bytes(data[i*4:i*4+4],'little')
    print('  ', (px>>16)&0xFF, (px>>8)&0xFF, px&0xFF)
