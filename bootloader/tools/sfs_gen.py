#!/usr/bin/env python3
"""
sfs_gen.py - Simple File System (SFS) image generator

Creates a binary SFS image from files in a source directory.
The image is written to the BIOS disk at LBA 800 during build.

SFS layout:
  Sector 0 (LBA 800):  Superblock
    magic[4] = "SFS\0"
    version  = 1 (uint16)
    file_count (uint16)
    data_start (uint32) = 817
    free_lba   (uint32)
    total_sectors (uint32)

  Sectors 1-16 (LBA 801-816): Directory
    16 entries per sector, 32 bytes each = 256 max entries
    Each entry: name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)

  Sectors 17+ (LBA 817+): File data (each file padded to sector boundary)
"""
import sys, os, struct

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <source_dir> <output_img>")
        sys.exit(1)

    src_dir = sys.argv[1]
    out_img = sys.argv[2]

    # Collect files from source directory
    files = []
    if os.path.isdir(src_dir):
        for name in sorted(os.listdir(src_dir)):
            fpath = os.path.join(src_dir, name)
            if os.path.isfile(fpath):
                data = open(fpath, 'rb').read()
                if len(name) >= 20:
                    print(f"Warning: filename '{name}' too long, truncating")
                    name = name[:19]
                files.append((name, data))

    print(f"SFS: packing {len(files)} files from {src_dir}")

    # Build the image
    SECTOR = 512
    ENTRY_SIZE = 32
    ENTRIES_PER_SECTOR = SECTOR // ENTRY_SIZE  # 16
    DIR_SECTORS = 16
    DATA_START_LBA = 817  # LBA 800 (super) + 17 (dir) = 817

    # Calculate file layout
    entries = []
    current_lba = DATA_START_LBA
    file_data = b''

    for name, data in files:
        size = len(data)
        sectors = (size + SECTOR - 1) // SECTOR
        if sectors == 0:
            sectors = 1
        # Pad data to sector boundary
        padded = data + b'\x00' * (sectors * SECTOR - size)
        entries.append((name, size, current_lba))
        file_data += padded
        current_lba += sectors
        print(f"  {name}: {size} bytes -> LBA {current_lba - sectors} ({sectors} sectors)")

    # Build superblock (512 bytes)
    sb = bytearray(SECTOR)
    sb[0:4] = b'SFS\x00'
    struct.pack_into('<H', sb, 4, 1)          # version
    struct.pack_into('<H', sb, 6, len(files))  # file_count
    struct.pack_into('<I', sb, 8, DATA_START_LBA)  # data_start
    struct.pack_into('<I', sb, 12, current_lba)    # free_lba
    struct.pack_into('<I', sb, 16, 207)            # total_sectors

    # Build directory (16 sectors = 8192 bytes)
    directory = bytearray(DIR_SECTORS * SECTOR)
    for i, (name, size, lba) in enumerate(entries):
        offset = i * ENTRY_SIZE
        name_bytes = name.encode('ascii')[:19]
        directory[offset:offset + len(name_bytes)] = name_bytes
        directory[offset + len(name_bytes)] = 0  # null terminator
        struct.pack_into('<I', directory, offset + 20, size)       # size at offset 20
        struct.pack_into('<I', directory, offset + 24, lba)        # start_lba at offset 24
        directory[offset + 28] = 0                                  # type = file (0)
        struct.pack_into('<H', directory, offset + 29, 0xFFFF)     # parent = root (0xFFFF)
        directory[offset + 31] = 0                                  # reserved

    # Write the complete image
    with open(out_img, 'wb') as f:
        f.write(sb)
        f.write(directory)
        f.write(file_data)

    total_size = len(sb) + len(directory) + len(file_data)
    total_sectors = (total_size + SECTOR - 1) // SECTOR
    print(f"SFS image: {out_img} ({total_size} bytes, {total_sectors} sectors)")
    print(f"  Superblock:  LBA 800")
    print(f"  Directory:   LBA 801-{800 + DIR_SECTORS}")
    print(f"  Data:        LBA {DATA_START_LBA}-{current_lba - 1}")

if __name__ == '__main__':
    main()
