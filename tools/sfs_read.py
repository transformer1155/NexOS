#!/usr/bin/env python3
import struct, sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# Superblock at LBA 800 (sector 512 bytes each)
sb_off = 800 * 512
magic = data[sb_off:sb_off+4]
if magic != b'SFS\x00':
    print("WRONG MAGIC!", magic)
    sys.exit(1)

version, count = struct.unpack_from('<HH', data, sb_off+4)
data_start = struct.unpack_from('<I', data, sb_off+8)[0]
free_lba = struct.unpack_from('<I', data, sb_off+12)[0]
total_sec = struct.unpack_from('<I', data, sb_off+16)[0]

print(f"SFS: v{version} files={count} data_start=LBA{data_start} free=LBA{free_lba} total={total_sec}")

# Directory entries (16 sectors = 256 entries, 32 bytes each)
dir_off = 801 * 512
found = []
for i in range(min(count, 256)):
    eoff = dir_off + i * 32
    name = data[eoff:eoff+20].split(b'\x00')[0].decode('utf-8', errors='replace')
    size, start, typ, parent = struct.unpack_from('<IIBB', data, eoff+20)
    found.append((name, size, start, typ))

# Sort by start LBA
found.sort(key=lambda x: x[2])
for name, size, start, typ in found:
    marker = " *** FOUND ***" if 'shell.mex' in name else ""
    print(f"  {name:30s} size={size:8d} start=LBA{start} type={typ}{marker}")
