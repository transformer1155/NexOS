#!/usr/bin/env python3
"""
make_data_vdi.py - Create a pre-formatted NexOS user-data disk as a
fixed (static) VirtualBox VDI image.

The VDI data region contains the same MKFS filesystem that
tools/make_data_vhd.py writes (superblock at LBA 512), so NexOS mounts
it automatically as a persistent user disk regardless of boot medium.

Usage:
    python3 tools/make_data_vdi.py [size_mb]
    # default size 8 MiB (>= 7.7 MiB required for DATA_DISK_SECTORS=15200)
Output:
    build/data.vdi  (VirtualBox fixed VDI; attach as IDE Secondary Master)
"""
import struct, sys, os, uuid

# ---- VDI container constants (VDI 1.1, matches qemu block/vdi.c) ----
VDI_SIGNATURE  = 0xbeda107f
VDI_VERSION    = 0x00010001
VDI_TYPE_STATIC = 2
HEADER_SIZE    = 512
BLOCK_SIZE     = 1 << 20            # 1 MiB blocks
SECTOR_SIZE    = 512

# ---- MKFS layout (must match kernel.cpp) ----
SUPER_LBA    = 512
TABLE_LBA    = 513
TABLE_SECT   = 16
DATA_LBA     = 529
DATA_SECTORS = 15200


def build_mkfs(size_mb):
    """Return a raw MKFS disk image (bytes) of size_mb MiB."""
    if size_mb < 8:
        print("size too small, using 8 MiB minimum")
        size_mb = 8
    total_sectors = size_mb * 1024 * 1024 // SECTOR_SIZE
    assert SUPER_LBA + 1 + TABLE_SECT + DATA_SECTORS <= total_sectors, \
        "disk too small for MKFS data area"
    img = bytearray(total_sectors * SECTOR_SIZE)
    # Superblock (struct Superblock in kernel.cpp)
    sb = struct.pack('<4sHHIII',
                     b'MKFS',          # magic
                     1,                # version
                     0,                # file_count
                     DATA_LBA,         # data_start
                     DATA_LBA,         # free_lba
                     DATA_SECTORS,     # total_sectors
                     )
    img[SUPER_LBA * SECTOR_SIZE: SUPER_LBA * SECTOR_SIZE + len(sb)] = sb
    # Table sectors stay zero (no files)
    return bytes(img)


def create_vdi(path, raw):
    disk_size = len(raw)
    nblocks = (disk_size + BLOCK_SIZE - 1) // BLOCK_SIZE
    disk_size = nblocks * BLOCK_SIZE

    blocks_offset = HEADER_SIZE
    bat_size = nblocks * 4
    offset_data = blocks_offset + bat_size
    # align data start to a sector (qemu-img behaviour)
    offset_data = (offset_data + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1)

    header = bytearray(HEADER_SIZE)
    text = b"<<< innotek VirtualBox Disk Image >>>\n"
    header[0:len(text)] = text
    struct.pack_into('<I', header, 0x40, VDI_SIGNATURE)
    struct.pack_into('<I', header, 0x44, VDI_VERSION)
    struct.pack_into('<I', header, 0x48, HEADER_SIZE)
    struct.pack_into('<I', header, 0x4c, VDI_TYPE_STATIC)
    struct.pack_into('<I', header, 0x50, 0)            # image flags
    struct.pack_into('<I', header, 0x54, 0)            # desc size
    struct.pack_into('<I', header, 0x58, blocks_offset)
    struct.pack_into('<I', header, 0x5c, offset_data)
    struct.pack_into('<I', header, 0x60, 0)            # cyls (reader recomputes)
    struct.pack_into('<I', header, 0x64, 16)           # heads
    struct.pack_into('<I', header, 0x68, 63)           # secs/track
    struct.pack_into('<I', header, 0x6c, SECTOR_SIZE)
    struct.pack_into('<Q', header, 0x70, disk_size)    # disk size (bytes)
    struct.pack_into('<I', header, 0x78, BLOCK_SIZE)   # block size
    struct.pack_into('<I', header, 0x7c, 0)            # block extra
    struct.pack_into('<I', header, 0x80, nblocks)      # blocks in image
    struct.pack_into('<I', header, 0x84, nblocks)      # blocks allocated
    u = uuid.uuid4().bytes
    header[0x88:0x98] = u                              # uuid create
    header[0x98:0xa8] = u                              # uuid modify
    # uuid_linkage / uuid_parent / comment left zero

    bat = bytearray(nblocks * 4)
    for i in range(nblocks):
        struct.pack_into('<I', bat, i * 4, i)          # identity block map

    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'wb') as f:
        f.write(header)
        f.write(bat)
        f.seek(offset_data)
        f.write(raw[:disk_size])


def main():
    size_mb = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    raw = build_mkfs(size_mb)
    out = os.path.join('build', 'data.vdi')
    create_vdi(out, raw)
    nblocks = (len(raw) + BLOCK_SIZE - 1) // BLOCK_SIZE
    print("Wrote %s (%d MiB disk, %d x 1MiB blocks, VDI fixed)"
          % (out, len(raw) // (1024 * 1024), nblocks))


if __name__ == '__main__':
    main()
