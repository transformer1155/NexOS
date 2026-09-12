#!/usr/bin/env bash
cd /mnt/d/MyOS/bootloader || exit 1
{
  echo "=== reloc types in bootx64_gf.so ==="
  readelf -r build/bootx64_gf.so 2>&1 | awk '{print $3}' | grep -E 'R_X86' | sort | uniq -c
  echo "=== reloc count ==="
  readelf -r build/bootx64_gf.so 2>&1 | grep -c 'R_X86'
  echo "=== PE headers (BOOTX64.EFI) ==="
  python3 - <<'PY'
import struct
d=open('build/BOOTX64.EFI','rb').read()
print("size", len(d))
e_lfanew=struct.unpack_from('<I', d, 0x3C)[0]
print("e_lfanew", hex(e_lfanew))
assert d[e_lfanew:e_lfanew+4]==b'PE\0\0'
coff=e_lfanew+4
machine, nsec = struct.unpack_from('<HH', d, coff)
print("machine", hex(machine), "sections", nsec)
opt=coff+20
magic=struct.unpack_from('<H', d, opt)[0]
print("opt magic", hex(magic))
image_base=struct.unpack_from('<Q', d, opt+24)[0]
print("ImageBase", hex(image_base))
# section table
sec=opt+struct.unpack_from('<H', d, coff+16)[0]
print("sections:")
for i in range(nsec):
    o=sec+i*40
    name=d[o:o+8].rstrip(b'\0').decode('latin1')
    vsize,vaddr,rsize,roff,_,_,_,chars=struct.unpack_from('<IIIIIIHH', d, o+8)
    print("  %-8s VA=%08x vsz=%08x raw=%08x chars=%08x" % (name, vaddr, vsize, rsize, chars))
PY
} > build/pe_check.txt 2>&1
