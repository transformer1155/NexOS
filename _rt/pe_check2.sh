#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
python3 - > build/pe_check2.txt 2>&1 <<'PY'
import struct
d=open('build/BOOTX64.EFI','rb').read()
e=struct.unpack_from('<I', d, 0x3C)[0]
coff=e+4
machine,nsec=struct.unpack_from('<HH', d, coff)
optsz=struct.unpack_from('<H', d, coff+16)[0]
coff_chars=struct.unpack_from('<H', d, coff+18)[0]
opt=coff+20
magic=struct.unpack_from('<H', d, opt)[0]
print("machine=%04x nsec=%d optsz=%d coff_chars=%04x magic=%04x" % (machine,nsec,optsz,coff_chars,magic))
sb=opt+24
image_base=struct.unpack_from('<Q', d, sb)[0]
sec_align,file_align=struct.unpack_from('<II', d, sb+8)
size_image=struct.unpack_from('<I', d, sb+24)[0]
size_headers=struct.unpack_from('<I', d, sb+28)[0]
checksum=struct.unpack_from('<I', d, sb+40)[0]
subsystem=struct.unpack_from('<H', d, sb+44)[0]
num_dirs=struct.unpack_from('<I', d, sb+76)[0]
print("ImageBase=%x SecAlign=%x FileAlign=%x SizeOfImage=%x SizeOfHeaders=%x Subsystem=%d Checksum=%x NumDirs=%d"
      % (image_base,sec_align,file_align,size_image,size_headers,subsystem,checksum,num_dirs))
sec=opt+optsz
for i in range(nsec):
    o=sec+i*40
    name=d[o:o+8].rstrip(b'\0').decode('latin1')
    vs,vaddr,rs,roff=struct.unpack_from('<IIII', d, o+8)
    ch=struct.unpack_from('<I', d, o+36)[0]
    print("  %-8s VA=%08x vsz=%08x raw=%08x chars=%08x" % (name,vaddr,vs,rs,ch))
dd=opt+112+5*8
rva,sz=struct.unpack_from('<II', d, dd)
print("RelocDir RVA=%x size=%x" % (rva,sz))
if rva:
    print("reloc bytes:", ' '.join('%02x'%b for b in d[rva:rva+16]))
PY
