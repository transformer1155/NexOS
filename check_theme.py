import struct
f = r'D:\MyOS\bootloader\csharp\apps\Shell\bin\Release\Shell.dll'
d = open(f, 'rb').read()
# search for ldc.i4 instructions with our color constants (little-endian int32)
vals = [0x35B6FF, 0x1E7FE0, 0x05216B, 0x218FD9, 0x5B86C4, 0xCFE3FF]
for v in vals:
    seq = bytes([0x20]) + struct.pack('<I', v)  # ldc.i4
    cnt = d.count(seq)
    print(f'0x{v:06X}: {cnt}')
# also search for raw int32 constants (e.g. in field initializers)
print('raw counts:')
for v in vals:
    cnt = d.count(struct.pack('<I', v))
    print(f'0x{v:06X}: {cnt}')
