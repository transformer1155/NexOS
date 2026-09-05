import struct, sys
f = r'D:\MyOS\bootloader\build\sfs.img'
data = open(f, 'rb').read()
SECTOR = 512
sb = data[:SECTOR]
magic = sb[:4]
version, count, data_start, free_lba, total_sectors = struct.unpack_from('<HHIII', sb, 4)
print(f'magic={magic} version={version} count={count} data_start={data_start} free={free_lba} total_sectors={total_sectors}')
dir_start = SECTOR  # file offset 512
print('Directory entries:')
for i in range(min(count, 256)):
    off = dir_start + i*32
    if off+32 > len(data): break
    raw = data[off:off+20]
    name = raw.split(b'\x00',1)[0].decode('ascii','replace')
    size, lba = struct.unpack_from('<II', data, off+20)
    ftype = data[off+28]
    parent = struct.unpack_from('<H', data, off+29)[0]
    if name or size or lba:
        print(f'  [{i}] "{name}" size={size} lba={lba} type={ftype} parent={parent}')
