#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
python3 - > build/pe_check3.txt 2>&1 <<'PY'
import struct
d=open('build/BOOTX64.EFI','rb').read()
e=struct.unpack_from('<I', d, 0x3C)[0]
coff=e+4
optsz=struct.unpack_from('<H', d, coff+16)[0]
opt=coff+20
image_base=struct.unpack_from('<Q', d, opt+24)[0]
sec_align=struct.unpack_from('<I', d, opt+32)[0]
file_align=struct.unpack_from('<I', d, opt+36)[0]
size_image=struct.unpack_from('<I', d, opt+56)[0]
size_headers=struct.unpack_from('<I', d, opt+60)[0]
checksum=struct.unpack_from('<I', d, opt+64)[0]
subsystem=struct.unpack_from('<H', d, opt+68)[0]
num_dirs=struct.unpack_from('<I', d, opt+108)[0]
print("ImageBase=%x SecAlign=%x FileAlign=%x" % (image_base,sec_align,file_align))
print("SizeOfImage=%x SizeOfHeaders=%x Checksum=%x Subsystem=%d NumDirs=%d"
      % (size_image,size_headers,checksum,subsystem,num_dirs))
for i in range(min(num_dirs,8)):
    rva,sz=struct.unpack_from('<II', d, opt+112+i*8)
    print("  dir[%d] RVA=%08x size=%x" % (i,rva,sz))
# .reloc section pointer-to-raw-data
nsec=struct.unpack_from('<H', d, coff+2)[0]
sec=opt+optsz
for i in range(nsec):
    o=sec+i*40
    name=d[o:o+8].rstrip(b'\0').decode('latin1')
    roff=struct.unpack_from('<I', d, o+20)[0]
    rs=struct.unpack_from('<I', d, o+16)[0]
    if name=='.reloc':
        print(".reloc file off=%x raw=%x bytes=%s" % (roff, rs, ' '.join('%02x'%b for b in d[roff:roff+min(rs,16)])))
print("total size=%x (SizeOfImage rounded=%x)" % (len(d), (size_image+sec_align-1)&~(sec_align-1)))
PY
