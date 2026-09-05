#!/usr/bin/env python3
"""Inspect BOOTX64.EFI PE header inside os.iso for real-hardware UEFI compatibility."""
import struct, sys

iso = open('build/os.iso', 'rb')
B = 2048


def rb(n):
    iso.seek(n * B)
    return iso.read(B)


def walk(buf, parent='/', lvl=0):
    off = 0
    out = []
    while off < len(buf):
        rl = buf[off]
        if rl == 0:
            off += 1
            continue
        rec = buf[off:off + rl]
        extent = struct.unpack('<I', rec[2:6])[0]
        dlen = struct.unpack('<I', rec[10:14])[0]
        flags = rec[25]
        nl = rec[32]
        name = rec[33:33 + nl].decode('latin1').split(';')[0]
        if name not in ('\x00', '\x01'):
            p = parent + name
            isd = bool(flags & 2)
            out.append((p, isd, extent, dlen))
            if isd and lvl < 4 and dlen > 0:
                out += walk(rb(extent)[:dlen], p + '/', lvl + 1)
        off += rl
    return out


pvd = rb(16)
rr = pvd[0x9C:0x9C + 34]
root_ext = struct.unpack('<I', rr[2:6])[0]
root_len = struct.unpack('<I', rr[10:14])[0]
files = walk(rb(root_ext)[:root_len])

cands = [f for f in files if f[0].upper().endswith('BOOTX64.EFI')]
if not cands:
    print("NO BOOTX64.EFI in iso")
    sys.exit(1)
_, _, ext, size = cands[0]
iso.seek(ext * B)
data = iso.read(size)
open('build/_extracted_BOOTX64.EFI', 'wb').write(data)
print("extracted BOOTX64.EFI:", size, "bytes")
print()

assert data[:2] == b'MZ', "not MZ"
pe_off = struct.unpack('<I', data[0x3C:0x40])[0]
assert data[pe_off:pe_off + 4] == b'PE\0\0', "no PE signature"
machine, nsec, _, _, _, optsz, chars = struct.unpack('<HHIIIHH', data[pe_off + 4:pe_off + 24])
print("Machine        = 0x%04X (%s)" % (machine, "x86_64 OK" if machine == 0x8664 else "WRONG"))
print("Sections       = %d" % nsec)
print("Characteristics= 0x%04X   RELOCS_STRIPPED=%s" % (chars, "YES <-- PROBLEM" if chars & 1 else "no"))

opt = pe_off + 24
magic = struct.unpack('<H', data[opt:opt + 2])[0]
print("OptHdr magic   = 0x%04X (%s)" % (magic, "PE32+ OK" if magic == 0x20b else "PE32 - wrong for x64"))
imagebase = struct.unpack('<Q', data[opt + 24:opt + 32])[0]
sec_align, file_align = struct.unpack('<II', data[opt + 32:opt + 40])
sizeimg, sizehdr = struct.unpack('<II', data[opt + 56:opt + 64])
subsys = struct.unpack('<H', data[opt + 68:opt + 70])[0]
dllchar = struct.unpack('<H', data[opt + 70:opt + 72])[0]
print("ImageBase      = 0x%X" % imagebase)
print("SectionAlign   = 0x%X   FileAlign = 0x%X" % (sec_align, file_align))
print("SizeOfImage    = 0x%X (%d)   SizeOfHeaders = 0x%X" % (sizeimg, sizeimg, sizehdr))
print("Subsystem      = %d (%s)" % (subsys, "EFI_APPLICATION OK" if subsys == 10 else "WRONG must be 10"))
print("DllCharacteristics = 0x%04X" % dllchar)

ndir = struct.unpack('<I', data[opt + 108:opt + 112])[0]
print("NumberOfRvaAndSizes = %d" % ndir)
dirs = opt + 112
names = ['Export', 'Import', 'Resource', 'Exception', 'Certificate', 'BaseReloc', 'Debug']
has_reloc = False
for i, nm in enumerate(names):
    if i < ndir:
        rva, sz = struct.unpack('<II', data[dirs + i * 8:dirs + i * 8 + 8])
        if rva or sz:
            print("  DataDir[%d] %-12s RVA=0x%X size=%d" % (i, nm, rva, sz))
            if nm == 'BaseReloc' and sz > 0:
                has_reloc = True
print("BaseReloc present: %s" % ("YES" if has_reloc else "NO <-- image cannot be relocated"))
print()
print("--- Sections ---")
sh = opt + optsz
maxend = 0
for i in range(nsec):
    e = data[sh + i * 40:sh + i * 40 + 40]
    nm = e[:8].rstrip(b'\0').decode('latin1')
    vs, va, rs, ra = struct.unpack('<IIII', e[8:24])
    fl = struct.unpack('<I', e[36:40])[0]
    print("  %-10s VA=0x%06X VSize=0x%06X RawPtr=0x%06X RawSize=0x%06X Flags=0x%08X"
          % (nm, va, vs, ra, rs, fl))
    if ra + rs > maxend:
        maxend = ra + rs
print()
print("file size = %d, max section raw end = %d, truncated=%s"
      % (len(data), maxend, "YES <-- PROBLEM" if maxend > len(data) else "no"))
