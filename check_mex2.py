import struct
f = r'D:\MyOS\bootloader\sfs_files\shell.mex'
d = open(f, 'rb').read()
# search raw little-endian int32 for the WallTop/WallBot constants
vals = [0x35B6FF, 0x1E7FE0, 0x05216B, 0x218FD9, 0x5B86C4, 0xCFE3FF, 0x0D57B3, 0x2E97F0, 0x17171B]
for v in vals:
    cnt = d.count(struct.pack('<I', v))
    print(f'0x{v:06X}: {cnt}')
