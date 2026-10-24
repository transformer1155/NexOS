#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
python3 - <<'EOF'
import struct
img=open('build/sfs.img','rb').read()
print('image size:', len(img))
print('magic[0:4]:', img[0:4])
ver,count=struct.unpack_from('<HH',img,4)
ds,free_lba,total=struct.unpack_from('<III',img,8)
print('version=%d file_count=%d data_start=%d free_lba=%d total=%d'%(ver,count,ds,free_lba,total))
# directory starts at sector 1
DIR_START=512
found=False
for e in range(count):
    off=DIR_START+e*32
    name=img[off:off+20].split(b'\x00')[0].decode('latin1',errors='replace')
    if not name: continue
    size,lba=struct.unpack_from('<II',img,off+20)
    if 'svm' in name or name.endswith('.drv'):
        print('  FOUND: %-20s size=%d lba=%d'%(name,size,lba))
        found=True
if not found: print('  NOT FOUND')
EOF
