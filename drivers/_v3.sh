#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS || exit 1
python3 - <<'EOF'
img=open('build/os_v2.img','rb').read()
print('image size: %d (%.2f MB)'%(len(img),len(img)/1048576))
def show(lba,n=8):
    o=lba*512
    return img[o:o+n].hex(' ')
print()
print('LBA0   boot magic :', show(0))
print('LBA2048 kernel64  :', show(2048))
print('LBA6144 SFS magic :', show(6144,4), '->', img[6144*512:6144*512+4])
print('LBA25600 linux_sfs:', show(25600,4), '->', img[25600*512:25600*512+4])
print()
# verify kernel32 tail not overwritten by kernel64 (kernel64 starts at 2048*512=1048576)
k32=open('build/kernel.bin','rb').read()
print('kernel32 size: %d, ends at offset %d'%(len(k32), 512+16384+len(k32)))
print('kernel64 starts at: 1048576')
print('OK no overlap' if 512+16384+len(k32) < 1048576 else '!! OVERLAP')
# check SFS at 6144 has svm.drv
SB=6144*512
import struct
magic=img[SB:SB+4]
ver,count=struct.unpack_from('<HH',img,SB+4)
print()
print('SFS@6144 magic=%r version=%d file_count=%d'%(magic,ver,count))
found=False
for e in range(count):
    off=SB+512+e*32
    name=img[off:off+20].split(b'\x00')[0].decode('latin1',errors='replace')
    if 'svm' in name:
        size,lba=struct.unpack_from('<II',img,off+20)
        print('  svm.drv: size=%d lba=%d'%(size,lba))
        found=True
print('  svm.drv PRESENT in image' if found else '  !! svm.drv MISSING')
EOF
