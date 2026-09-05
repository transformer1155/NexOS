#!/usr/bin/env python3
"""
gen_reloc.py - Generate PE32+ .reloc section from ELF R_X86_64_RELATIVE
               relocations and patch it into the PE32+ binary.

Usage:
    python3 gen_reloc.py <bootx64.so> <BOOTX64.EFI>

This reads the ELF .rela section, converts R_X86_64_RELATIVE entries
to IMAGE_REL_BASED_DIR64 base relocations, groups them by 4K page,
and patches the .reloc section into the PE32+ output file.

PE32+ .reloc section format:
    IMAGE_BASE_RELOCATION {
        uint32_t PageRVA;     // RVA of the 4K page needing fixups
        uint32_t BlockSize;   // Total size of this block (including header)
        uint16_t TypeOffset[1]; // Variable-length array of fixups
    }
    ... (repeated for each page)
    Terminated by a block with PageRVA=0 (optional, usually not needed)

Each TypeOffset:
    bits 15-12: Type (0=ABSOLUTE/padding, 10=DIR64 for x86_64)
    bits 11-0:  Offset from PageRVA
"""

import struct
import sys
import os

# ELF constants
SHT_RELA = 4
R_X86_64_RELATIVE = 8

# PE constants
IMAGE_REL_BASED_DIR64 = 0xA
IMAGE_REL_BASED_ABSOLUTE = 0
PAGE_SIZE = 0x1000


def read_elf_relocations(elf_path):
    """Read R_X86_64_RELATIVE entries from the ELF .rela section."""
    with open(elf_path, 'rb') as f:
        data = f.read()

    # Parse ELF header (64-bit)
    if data[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")

    ei_class = data[4]  # 1=32-bit, 2=64-bit
    if ei_class != 2:
        raise ValueError("Not a 64-bit ELF file")

    # Parse ELF header fields
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]  # Section header offset
    e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]  # Section header entry size
    e_shnum = struct.unpack_from('<H', data, 0x3C)[0]  # Number of sections
    e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]  # Section name string table index

    # Read section headers
    sections = []
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        sh_name = struct.unpack_from('<I', data, sh_off)[0]
        sh_type = struct.unpack_from('<I', data, sh_off + 4)[0]
        sh_flags = struct.unpack_from('<Q', data, sh_off + 8)[0]
        sh_addr = struct.unpack_from('<Q', data, sh_off + 16)[0]
        sh_offset = struct.unpack_from('<Q', data, sh_off + 24)[0]
        sh_size = struct.unpack_from('<Q', data, sh_off + 32)[0]
        sections.append((sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size))

    # Read section name string table
    shstrtab_off = e_shoff + e_shstrndx * e_shentsize
    shstrtab_sh_offset = struct.unpack_from('<Q', data, shstrtab_off + 24)[0]

    relocations = []

    # Find .rela section
    for sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size in sections:
        # Get section name
        name_end = data.find(b'\x00', shstrtab_sh_offset + sh_name)
        name = data[shstrtab_sh_offset + sh_name:name_end].decode('ascii', errors='replace')

        if name == '.rela' and sh_type == SHT_RELA:
            # Parse RELA entries
            entry_size = 24  # 3 * 8 bytes (offset, info, addend)
            num_entries = sh_size // entry_size

            for j in range(num_entries):
                entry_off = sh_offset + j * entry_size
                r_offset = struct.unpack_from('<Q', data, entry_off)[0]
                r_info = struct.unpack_from('<Q', data, entry_off + 8)[0]
                r_addend = struct.unpack_from('<Q', data, entry_off + 16)[0]
                r_type = r_info & 0xFFFFFFFF

                if r_type == R_X86_64_RELATIVE:
                    relocations.append((r_offset, r_addend))

            break

    print(f"[RELOC] Found {len(relocations)} R_X86_64_RELATIVE entries in .rela")
    return relocations


def generate_reloc_section(relocations):
    """
    Generate PE32+ .reloc section data.
    Groups relocations by 4K page and creates IMAGE_BASE_RELOCATION blocks.
    """
    if not relocations:
        print("[RELOC] WARNING: No relocations to process!")
        return b''

    # Group by page
    pages = {}
    for r_offset, r_addend in relocations:
        page = r_offset & ~(PAGE_SIZE - 1)
        offset_in_page = r_offset & (PAGE_SIZE - 1)
        if page not in pages:
            pages[page] = []
        pages[page].append(offset_in_page)

    # Sort pages
    sorted_pages = sorted(pages.keys())

    # Build .reloc section data
    reloc_data = bytearray()

    for page in sorted_pages:
        offsets = sorted(set(pages[page]))  # Remove duplicates, sort

        # Calculate block size: 8 (header) + 2 * num_entries, aligned to 4
        block_size = 8 + 2 * len(offsets)
        if block_size % 4 != 0:
            block_size += 4 - (block_size % 4)

        # How many entries fit (including padding)
        num_type_offsets = (block_size - 8) // 2

        reloc_data += struct.pack('<II', page, block_size)

        for offset in offsets:
            type_offset = (IMAGE_REL_BASED_DIR64 << 12) | (offset & 0xFFF)
            reloc_data += struct.pack('<H', type_offset)

        # Add padding with ABSOLUTE entries (type 0) if needed
        remaining = num_type_offsets - len(offsets)
        for _ in range(remaining):
            reloc_data += struct.pack('<H', IMAGE_REL_BASED_ABSOLUTE << 12)

    print(f"[RELOC] Generated .reloc section: {len(reloc_data)} bytes across {len(sorted_pages)} pages")
    for page in sorted_pages:
        print(f"[RELOC]   Page 0x{page:08X}: {len(pages[page])} fixups")

    return bytes(reloc_data)


def patch_pe32_reloc(pe_path, reloc_data):
    """
    Patch the PE32+ binary to include the .reloc section.
    """
    with open(pe_path, 'rb') as f:
        pe_data = bytearray(f.read())

    # ---- Parse DOS header ----
    if pe_data[:2] != b'MZ':
        raise ValueError("Not a PE file (no MZ signature)")
    e_lfanew = struct.unpack_from('<I', pe_data, 0x3C)[0]

    # ---- Parse PE signature ----
    pe_sig = e_lfanew + 4
    if pe_data[pe_sig - 4:pe_sig] != b'PE\x00\x00':
        raise ValueError("Not a PE file (no PE signature)")

    # ---- COFF header (20 bytes) ----
    coff = pe_sig  # base of COFF header
    num_sections = struct.unpack_from('<H', pe_data, coff + 2)[0]
    opt_hdr_size = struct.unpack_from('<H', pe_data, coff + 16)[0]

    # ---- Optional header ----
    opt = pe_sig + 20  # base of Optional Header
    magic = struct.unpack_from('<H', pe_data, opt)[0]
    if magic != 0x20B:  # PE32+
        raise ValueError(f"Not a PE32+ file (magic=0x{magic:04X})")

    # PE32+ field offsets from start of Optional Header:
    #   0x00: Magic (2)
    #   0x02: MajorLinkerVersion (1), MinorLinkerVersion (1)
    #   0x04: SizeOfCode (4)
    #   0x08: SizeOfInitializedData (4)
    #   0x0C: SizeOfUninitializedData (4)
    #   0x10: AddressOfEntryPoint (4)
    #   0x14: BaseOfCode (4)
    #   0x18: ImageBase (8)
    #   0x20: SectionAlignment (4)
    #   0x24: FileAlignment (4)
    #   0x28: MajorOperatingSystemVersion (2) ... etc
    #   0x38: SizeOfImage (4)
    #   0x3C: SizeOfHeaders (4)
    #   0x40: CheckSum (4)
    #   0x44: Subsystem (2)
    #   0x46: DllCharacteristics (2)
    #   0x48: SizeOfStackReserve (8)
    #   0x50: SizeOfStackCommit (8)
    #   0x58: SizeOfHeapReserve (8)
    #   0x60: SizeOfHeapCommit (8)
    #   0x68: LoaderFlags (4)
    #   0x6C: NumberOfRvaAndSizes (4)
    #   0x70: Data Directory (16 entries * 8 = 128 bytes)

    section_alignment = struct.unpack_from('<I', pe_data, opt + 0x20)[0]
    file_alignment = struct.unpack_from('<I', pe_data, opt + 0x24)[0]
    size_of_image = struct.unpack_from('<I', pe_data, opt + 0x38)[0]
    num_data_dir = struct.unpack_from('<I', pe_data, opt + 0x6C)[0]

    # Data Directory base
    dd_base = opt + 0x70

    # Base Relocation Directory is entry 5
    base_reloc_dir_offset = dd_base + 5 * 8
    old_reloc_rva = struct.unpack_from('<I', pe_data, base_reloc_dir_offset)[0]
    old_reloc_size = struct.unpack_from('<I', pe_data, base_reloc_dir_offset + 4)[0]

    # Section table starts after the optional header
    section_table_offset = opt + opt_hdr_size

    # Read section headers
    sections = []
    for i in range(num_sections):
        s_off = section_table_offset + i * 40
        s_name = pe_data[s_off:s_off+8].rstrip(b'\x00').decode('ascii', errors='replace')
        s_vsize = struct.unpack_from('<I', pe_data, s_off + 8)[0]
        s_rva = struct.unpack_from('<I', pe_data, s_off + 12)[0]
        s_rsize = struct.unpack_from('<I', pe_data, s_off + 16)[0]
        s_ptr = struct.unpack_from('<I', pe_data, s_off + 20)[0]
        s_chars = struct.unpack_from('<I', pe_data, s_off + 36)[0]
        sections.append((s_name, s_vsize, s_rva, s_rsize, s_ptr, s_chars))

    print(f"[RELOC] PE32+ sections:")
    for name, vsize, rva, rsize, ptr, chars in sections:
        print(f"[RELOC]   {name}: RVA=0x{rva:08X}, VSize=0x{vsize:X}, FilePtr=0x{ptr:X}, FileSize=0x{rsize:X}")

    print(f"[RELOC] Old Base Relocation Directory: RVA=0x{old_reloc_rva:08X}, Size=0x{old_reloc_size:X}")

    # Find the last section by file pointer (raw data end)
    last_section = max(sections, key=lambda s: s[0])  # By section name
    # Actually, find the one with the highest file pointer + raw size
    # Skip sections with empty names (they are placeholders)
    real_sections = [s for s in sections if s[0] and s[3] > 0]
    if not real_sections:
        # Fallback: use all sections
        real_sections = sections

    # Find the section with the highest file pointer for its raw data
    last_section = max(real_sections, key=lambda s: s[3] + s[4])  # raw_size + file_ptr
    last_name, last_vsize, last_rva, last_rsize, last_ptr, last_chars = last_section

    # Calculate end of last section's raw data
    last_end = last_ptr + last_rsize

    # Align to file alignment
    if file_alignment > 0:
        new_file_offset = (last_end + file_alignment - 1) // file_alignment * file_alignment
    else:
        new_file_offset = last_end

    # The new .reloc section RVA
    new_reloc_rva = (last_rva + last_vsize + section_alignment - 1) // section_alignment * section_alignment
    new_reloc_size = len(reloc_data)
    new_reloc_file_size = (new_reloc_size + file_alignment - 1) // file_alignment * file_alignment if file_alignment > 0 else new_reloc_size

    print(f"[RELOC] New .reloc: RVA=0x{new_reloc_rva:08X}, FileSize=0x{new_reloc_file_size:X}, FileOffset=0x{new_file_offset:X}")

    # Pad the PE data to the new file offset
    while len(pe_data) < new_file_offset:
        pe_data.append(0)

    # DEBUG: check data before writing
    print(f"[RELOC] DEBUG: new_file_offset=0x{new_file_offset:X}, len(pe_data)={len(pe_data)}")
    print(f"[RELOC] DEBUG: reloc_data[:16] = {reloc_data[:16].hex()}")
    print(f"[RELOC] DEBUG: pe_data at offset before write: {pe_data[new_file_offset:new_file_offset+8].hex()}")

    # Write the .reloc data at the new file offset
    pe_data[new_file_offset:new_file_offset + new_reloc_size] = reloc_data

    # DEBUG: verify after write
    print(f"[RELOC] DEBUG: pe_data at offset after write: {pe_data[new_file_offset:new_file_offset+8].hex()}")

    # Update the Base Relocation Directory entry
    struct.pack_into('<I', pe_data, base_reloc_dir_offset, new_reloc_rva)
    struct.pack_into('<I', pe_data, base_reloc_dir_offset + 4, new_reloc_size)

    # Add a new section header for .reloc
    new_section_offset = section_table_offset + num_sections * 40

    # Ensure there's space for the new section header
    while len(pe_data) < new_section_offset + 40:
        pe_data.append(0)

    # Write the new section header
    pe_data[new_section_offset:new_section_offset+8] = b'.reloc\0\0'
    struct.pack_into('<I', pe_data, new_section_offset + 8, new_reloc_size)       # VirtualSize
    struct.pack_into('<I', pe_data, new_section_offset + 12, new_reloc_rva)       # VirtualAddress
    struct.pack_into('<I', pe_data, new_section_offset + 16, new_reloc_file_size) # SizeOfRawData
    struct.pack_into('<I', pe_data, new_section_offset + 20, new_file_offset)     # PointerToRawData
    struct.pack_into('<I', pe_data, new_section_offset + 24, 0)  # PointerToRelocations
    struct.pack_into('<I', pe_data, new_section_offset + 28, 0)  # PointerToLineNumbers
    struct.pack_into('<H', pe_data, new_section_offset + 32, 0)  # NumberOfRelocations
    struct.pack_into('<H', pe_data, new_section_offset + 34, 0)  # NumberOfLineNumbers
    # Characteristics: INITIALIZED_DATA | MEM_READ | MEM_DISCARDABLE
    struct.pack_into('<I', pe_data, new_section_offset + 36, 0x42000040)

    # Update the number of sections in the COFF header
    struct.pack_into('<H', pe_data, coff + 2, num_sections + 1)

    # Fix PE characteristics:
    #   - Clear IMAGE_FILE_RELOCS_STRIPPED (bit 0) — we now have a .reloc section
    #   - Set   IMAGE_FILE_LARGE_ADDRESS_AWARE (bit 5) — required for 64-bit
    old_chars = struct.unpack_from('<H', pe_data, coff + 18)[0]
    new_chars = (old_chars & ~0x0001) | 0x0020
    struct.pack_into('<H', pe_data, coff + 18, new_chars)
    print(f"[RELOC]   Characteristics: 0x{old_chars:04X} -> 0x{new_chars:04X}")

    # Update SizeOfImage
    new_size_of_image = new_reloc_rva + max(new_reloc_size, new_reloc_file_size)
    new_size_of_image = (new_size_of_image + section_alignment - 1) // section_alignment * section_alignment
    struct.pack_into('<I', pe_data, opt + 0x38, new_size_of_image)

    # Write the patched PE file
    with open(pe_path, 'wb') as f:
        f.write(pe_data)

    print(f"[RELOC] Patched PE32+ binary: {pe_path}")
    print(f"[RELOC]   Sections: {num_sections} -> {num_sections + 1}")
    print(f"[RELOC]   SizeOfImage: 0x{size_of_image:X} -> 0x{new_size_of_image:X}")
    print(f"[RELOC]   Base Relocation Directory: RVA=0x{new_reloc_rva:08X}, Size=0x{new_reloc_size:X}")

    return True


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <bootx64.so> <BOOTX64.EFI>")
        sys.exit(1)

    elf_path = sys.argv[1]
    pe_path = sys.argv[2]

    if not os.path.exists(elf_path):
        print(f"Error: ELF file not found: {elf_path}")
        sys.exit(1)
    if not os.path.exists(pe_path):
        print(f"Error: PE32+ file not found: {pe_path}")
        sys.exit(1)

    # Step 1: Read ELF relocations
    relocations = read_elf_relocations(elf_path)

    # Step 2: Generate .reloc section
    reloc_data = generate_reloc_section(relocations)

    if not reloc_data:
        print("[RELOC] No .reloc data generated, nothing to patch.")
        sys.exit(0)

    # Step 3: Patch PE32+ binary
    patch_pe32_reloc(pe_path, reloc_data)

    print("[RELOC] Done!")


if __name__ == '__main__':
    main()