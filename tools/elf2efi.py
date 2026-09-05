#!/usr/bin/env python3
"""
elf2efi.py - Convert a position-independent ELF64 shared object (.so)
             into a valid PE32+ UEFI application (BOOTX64.EFI).

This is used because the native x86_64-elf binutils on this machine
cannot emit PE targets (no efi-app-x86-64 / pei-x86-64 support) and the
host MSYS2 toolchain/mtools are not installed.  The loader objects are
compiled -fPIC -fvisibility=hidden -fno-plt and linked with -Bsymbolic
and -e _start, which yields a fully RIP-relative (reloc-free) image.

Because the image is fully PIC, no base relocations are required.  We
still emit a minimal *valid* .reloc section (a single terminator block,
PageRVA=0 / BlockSize=0) and do NOT set IMAGE_FILE_RELOCS_STRIPPED, so
the UEFI PE loader is free to load the image at any address and the
relocation pass becomes a no-op.  This is the documented EDK2-compatible
way to describe a reloc-free image.
"""
import struct
import sys
import os
import time

SHF_ALLOC      = 0x2
SHT_PROGBITS   = 1
SHT_NOBITS     = 8
SHT_SYMTAB     = 2

# Sections we carry into the PE (in ELF RVA order).
SECTIONS_TO_EMIT = ['.text', '.rodata', '.sdata', '.data', '.bss']

SECT_CHARS = {
    '.text':   0x60000020,   # CODE | MEM_EXECUTE | MEM_READ
    '.rodata': 0x40000040,   # INITIALIZED_DATA | MEM_READ
    '.sdata':  0xC0000040,   # INITIALIZED_DATA | MEM_READ | MEM_WRITE
    '.data':   0xC0000040,
    '.bss':    0xC0000080,   # INITIALIZED_DATA | MEM_READ | MEM_WRITE | UNINITIALIZED
    '.reloc':  0x42000040,   # INITIALIZED_DATA | MEM_READ | MEM_DISCARDABLE
}

FILE_ALIGN = 0x200
SECT_ALIGN = 0x1000


def parse_elf(data):
    if data[:4] != b'\x7fELF':
        raise ValueError("not ELF")
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]

    secs = []
    for i in range(e_shnum):
        b = e_shoff + i * e_shentsize
        secs.append({
            'name_off': struct.unpack_from('<I', data, b)[0],
            'type':     struct.unpack_from('<I', data, b + 4)[0],
            'flags':    struct.unpack_from('<Q', data, b + 8)[0],
            'addr':     struct.unpack_from('<Q', data, b + 16)[0],
            'offset':   struct.unpack_from('<Q', data, b + 24)[0],
            'size':     struct.unpack_from('<Q', data, b + 32)[0],
            'link':     struct.unpack_from('<I', data, b + 40)[0],
        })
    shstr = secs[e_shstrndx]
    base = shstr['offset']

    def get_name(off):
        end = data.find(b'\x00', base + off)
        return data[base + off:end].decode('ascii', errors='replace')

    for s in secs:
        s['name'] = get_name(s['name_off'])
    return secs


def find_entry(data, secs):
    """Return the entry's raw ELF addr (caller applies the RVA shift).

    NOTE: the entry address may legitimately be 0 (e.g. the linker placed
    .text at virtual address 0 and _start is its first byte).  So this
    function returns None to mean "not found" and 0 to mean "found at 0".
    """
    e_entry = struct.unpack_from('<Q', data, 0x18)[0]
    if e_entry:
        return e_entry & 0xFFFFFFFF
    for s in secs:
        if s['type'] == SHT_SYMTAB and s['name'] == '.symtab':
            strs = secs[s['link']]
            count = s['size'] // 24
            for j in range(count):
                o = s['offset'] + j * 24
                st_name = struct.unpack_from('<I', data, o)[0]
                st_value = struct.unpack_from('<Q', data, o + 8)[0]
                nend = data.find(b'\x00', strs['offset'] + st_name)
                nm = data[strs['offset'] + st_name:nend].decode('ascii', errors='replace')
                if nm == '_start':
                    return st_value & 0xFFFFFFFF
    return None


def put_section(out, off, name, rva, vsize, raw_size, file_off, chars):
    nb = name.encode('ascii')[:8]
    out[off:off + 8] = nb + b'\x00' * (8 - len(nb))
    struct.pack_into('<I', out, off + 8, vsize & 0xFFFFFFFF)       # VirtualSize
    struct.pack_into('<I', out, off + 12, rva & 0xFFFFFFFF)        # VirtualAddress
    struct.pack_into('<I', out, off + 16, raw_size & 0xFFFFFFFF)   # SizeOfRawData
    struct.pack_into('<I', out, off + 20, file_off & 0xFFFFFFFF)   # PointerToRawData
    struct.pack_into('<I', out, off + 24, 0)                       # PointerToRelocations
    struct.pack_into('<I', out, off + 28, 0)                       # PointerToLineNumbers
    struct.pack_into('<H', out, off + 32, 0)                       # NumberOfRelocations
    struct.pack_into('<H', out, off + 34, 0)                       # NumberOfLineNumbers
    struct.pack_into('<I', out, off + 36, chars)                   # Characteristics


def pe_checksum(buf):
    if len(buf) & 1:
        buf = buf + b'\x00'
    s = 0
    for i in range(0, len(buf), 2):
        s += struct.unpack_from('<H', buf, i)[0]
        s = (s & 0xFFFF) + (s >> 16)
    s += len(buf)
    s = (s & 0xFFFF) + (s >> 16)
    return s & 0xFFFF


def main():
    if len(sys.argv) < 3:
        print("Usage: elf2efi.py <input.so> <output.EFI>")
        sys.exit(1)
    elf_path, out_path = sys.argv[1], sys.argv[2]
    with open(elf_path, 'rb') as f:
        data = f.read()

    secs = parse_elf(data)
    entry_raw = find_entry(data, secs)
    if entry_raw is None:
        raise SystemExit("ERROR: could not resolve entry point (_start)")

    emit = [s for s in secs if s['name'] in SECTIONS_TO_EMIT]
    emit.sort(key=lambda s: s['addr'])

    # ---- Build the raw section data blobs ----
    section_rows = []   # (name, rva, raw_bytes, vsize, is_bss)
    for s in emit:
        is_bss = (s['type'] == SHT_NOBITS)
        if is_bss:
            raw = b''
            vsize = s['size']
        else:
            raw = data[s['offset']:s['offset'] + s['size']]
            vsize = len(raw)
        section_rows.append((s['name'], s['addr'], raw, vsize, is_bss))
        print(f"[ELF2EFI] section {s['name']}: RVA=0x{s['addr']:08X} "
              f"vsize=0x{vsize:X} raw=0x{len(raw):X} {'BSS' if is_bss else ''}")

    # ---- RVA base shift ----
    # The ELF .so was linked with sections starting at virtual address 0,
    # which collides with the PE headers (RVA 0..SizeOfHeaders).  Shift every
    # section (and the entry) up by RVA_BASE so the first section begins at a
    # SectionAlignment-aligned address above the headers.  RIP-relative code
    # and the absolute physical addrs used at runtime (0x10000/0x5000/...) are
    # unaffected by this loader-side virtual layout choice.
    def size_of_headers_tmp():
        # provisional: 64(DOS)+64(stub)+4(PESIG)+20(COFF)+240(OPTHDR) + secs*40
        # recomputed precisely below, but a lower bound of 0x400 is enough
        # to keep the first section above the headers.
        return 0x400
    min_addr = min(rva for (_, rva, _, _, _) in section_rows)
    rva_base = ((size_of_headers_tmp() + SECT_ALIGN - 1) & ~(SECT_ALIGN - 1))
    shift = rva_base - min_addr
    section_rows = [(name, rva + shift, raw, vsize, is_bss)
                    for (name, rva, raw, vsize, is_bss) in section_rows]
    entry_rva = (entry_raw + shift) & 0xFFFFFFFF
    print(f"[ELF2EFI] RVA shift=0x{shift:X} (min_addr=0x{min_addr:X}) "
          f"-> entry RVA=0x{entry_rva:08X}")

    # ---- Minimal valid .reloc (terminator block only) ----
    reloc_data = struct.pack('<II', 0, 0)   # PageRVA=0, BlockSize=0 -> terminator
    max_rva_end = max(rva + vsize for (_, rva, _, vsize, _) in section_rows)
    reloc_rva = (max_rva_end + SECT_ALIGN - 1) & ~(SECT_ALIGN - 1)
    reloc_vsize = len(reloc_data)            # 8
    section_rows.append(('.reloc', reloc_rva, reloc_data, reloc_vsize, False))
    print(f"[ELF2EFI] .reloc: RVA=0x{reloc_rva:08X} (terminator block, reloc-free)")

    n_sections = len(section_rows)
    hdr_size = 64 + 64 + 4 + 20 + 240 + n_sections * 40
    size_of_headers = (hdr_size + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
    print(f"[ELF2EFI] SizeOfHeaders=0x{size_of_headers:X} n_sections={n_sections}")

    # Summary sizes
    size_of_code = sum(len(raw) for (name, _, raw, _, is_bss) in section_rows
                       if not is_bss and SECT_CHARS[name] == 0x60000020)
    size_of_init = sum((len(raw) if not is_bss else 0)
                       for (_, _, raw, _, is_bss) in section_rows)
    size_of_uninit = sum(vsize for (name, _, _, vsize, is_bss) in section_rows
                         if is_bss and name != '.reloc')

    # ---- File layout ----
    cur = size_of_headers
    file_rows = []   # (raw, rva, vsize, name, chars, raw_size, file_off)
    for (name, rva, raw, vsize, is_bss) in section_rows:
        cur = (cur + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
        raw_size = len(raw)
        file_rows.append((raw, rva, vsize, name, SECT_CHARS[name], raw_size, cur))
        cur += raw_size

    size_of_image = (reloc_rva + reloc_vsize + SECT_ALIGN - 1) & ~(SECT_ALIGN - 1)

    IMAGE_BASE = 0x400000

    # ---- Assemble the file ----
    out = bytearray(cur)
    out[0:2] = b'MZ'
    struct.pack_into('<I', out, 0x3C, 0x80)   # e_lfanew
    out[0x80:0x84] = b'PE\x00\x00'
    coff = 0x84       # COFF follows immediately after the 4-byte PE signature
    struct.pack_into('<H', out, coff + 0, 0x8664)        # Machine AMD64
    struct.pack_into('<H', out, coff + 2, n_sections)    # NumberOfSections
    struct.pack_into('<I', out, coff + 4, int(time.time()))  # TimeDateStamp
    struct.pack_into('<I', out, coff + 8, 0)             # PointerToSymbolTable
    struct.pack_into('<I', out, coff + 12, 0)            # NumberOfSymbols
    struct.pack_into('<H', out, coff + 16, 240)          # SizeOfOptionalHeader
    # Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    # (deliberately NOT RELOCS_STRIPPED so the loader can relocate us)
    struct.pack_into('<H', out, coff + 18, 0x0022)

    opt = coff + 20
    struct.pack_into('<H', out, opt + 0x00, 0x20B)       # Magic PE32+
    struct.pack_into('<B', out, opt + 0x02, 0)           # MajorLinkerVersion
    struct.pack_into('<B', out, opt + 0x03, 0)           # MinorLinkerVersion
    struct.pack_into('<I', out, opt + 0x04, size_of_code)     # SizeOfCode
    struct.pack_into('<I', out, opt + 0x08, size_of_init)     # SizeOfInitializedData
    struct.pack_into('<I', out, opt + 0x0C, size_of_uninit)   # SizeOfUninitializedData
    struct.pack_into('<I', out, opt + 0x10, entry_rva & 0xFFFFFFFF)  # EntryPoint
    struct.pack_into('<I', out, opt + 0x14, section_rows[0][1] & 0xFFFFFFFF)  # BaseOfCode
    struct.pack_into('<Q', out, opt + 0x18, IMAGE_BASE)  # ImageBase
    struct.pack_into('<I', out, opt + 0x20, SECT_ALIGN)  # SectionAlignment
    struct.pack_into('<I', out, opt + 0x24, FILE_ALIGN)  # FileAlignment
    struct.pack_into('<H', out, opt + 0x28, 1)           # OS major
    struct.pack_into('<H', out, opt + 0x2A, 0)           # OS minor
    struct.pack_into('<H', out, opt + 0x2C, 0)           # Image major
    struct.pack_into('<H', out, opt + 0x2E, 0)           # Image minor
    struct.pack_into('<H', out, opt + 0x30, 1)           # Subsystem major
    struct.pack_into('<H', out, opt + 0x32, 0)           # Subsystem minor
    struct.pack_into('<I', out, opt + 0x34, 0)           # Win32VersionValue
    struct.pack_into('<I', out, opt + 0x38, size_of_image)   # SizeOfImage
    struct.pack_into('<I', out, opt + 0x3C, size_of_headers) # SizeOfHeaders
    struct.pack_into('<I', out, opt + 0x40, 0)           # CheckSum (filled later)
    struct.pack_into('<H', out, opt + 0x44, 10)          # Subsystem = EFI_APPLICATION
    struct.pack_into('<H', out, opt + 0x46, 0x0020)      # DllCharacteristics = LARGE_ADDRESS_AWARE
    struct.pack_into('<Q', out, opt + 0x48, 0x100000)    # SizeOfStackReserve
    struct.pack_into('<Q', out, opt + 0x50, 0x1000)      # SizeOfStackCommit
    struct.pack_into('<Q', out, opt + 0x58, 0x100000)    # SizeOfHeapReserve
    struct.pack_into('<Q', out, opt + 0x60, 0x1000)      # SizeOfHeapCommit
    struct.pack_into('<I', out, opt + 0x68, 0)           # LoaderFlags
    struct.pack_into('<I', out, opt + 0x6C, 16)          # NumberOfRvaAndSizes
    # Data directories (16 * 8).  Base Relocation dir (index 5) points at .reloc.
    dd_base = opt + 0x70
    struct.pack_into('<I', out, dd_base + 5 * 8 + 0, reloc_rva)
    struct.pack_into('<I', out, dd_base + 5 * 8 + 4, reloc_vsize)

    # ---- Section table ----
    sect_tbl = opt + 240
    for row_idx, (raw, rva, vsize, name, chars, raw_size, file_off) in enumerate(file_rows):
        put_section(out, sect_tbl + row_idx * 40, name, rva, vsize,
                    raw_size, file_off, chars)
        if raw_size:
            out[file_off:file_off + raw_size] = raw

    # ---- Checksum ----
    cs = pe_checksum(bytes(out))
    struct.pack_into('<I', out, opt + 0x40, cs)

    with open(out_path, 'wb') as f:
        f.write(out)
    print(f"[ELF2EFI] wrote {out_path} ({len(out)} bytes), entry RVA=0x{entry_rva:X}, "
          f"checksum=0x{cs:X}")


if __name__ == '__main__':
    main()
