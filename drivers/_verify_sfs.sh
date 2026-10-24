#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== 确认 svm.drv 在 SFS 目录表中 ==='
python3 - <<'EOF'
import struct
img=open('build/sfs.img','rb').read()
SB=800*512
magic=img[SB:SB+4]
print('superblock magic:', magic)
ver,count=struct.unpack_from('<HH',img,SB+4)
print('version=%d file_count=%d'%(ver,count))
found=False
for s in range(16):
    base=(801+s)*512
    for e in range(16):
        off=base+e*32
        name=img[off:off+20].split(b'\x00')[0].decode('latin1')
        if not name: continue
        size,lba=struct.unpack_from('<II',img,off+20)
        if 'svm' in name or name.endswith('.drv'):
            print('  FOUND: %-20s size=%d lba=%d'%(name,size,lba))
            found=True
if not found: print('  !! svm.drv NOT FOUND in directory')
EOF
