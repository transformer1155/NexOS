#!/usr/bin/env python3
"""
make_data_vhd.py - Create a pre-formatted NexOS user-data disk (raw image).

The disk is formatted as MKFS at LBA 512 (super), 513-528 (table), 529+ (data).
NexOS mounts it automatically when a secondary ATA disk (0x170) is present,
so all user files persist across reboots regardless of the boot medium.

Usage:
    python3 tools/make_data_vhd.py [size_mb]
    # default size 8 MiB (>= 7.7 MiB required for DATA_DISK_SECTORS=15200)
Output:
    build/data.vhd  (raw image; attach as IDE Secondary Master in QEMU/VirtualBox)
"""
import struct, sys, os

def main():
    size_mb = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    if size_mb < 8:
        print("size too small, using 8 MiB minimum")
        size_mb = 8
    total_sectors = size_mb * 1024 * 1024 // 512
    # MKFS layout (must match kernel.cpp -- the 32-bit kernel the Makefile
    # compiles uses SUPER=900/DATA=917; LBA 512 would sit inside kernel32.bin)
    SUPER_LBA    = 900   # MKFS_SUPER_LBA in kernel.cpp
    TABLE_LBA    = 901   # MKFS_TABLE_LBA
    TABLE_SECT = 16
    DATA_LBA     = 917   # MKFS_DATA_LBA
    DATA_SECTORS = 15200 # DATA_DISK_SECTORS in kernel.cpp (dedicated data disk)
    MKFS_VERSION = 2     # must match MKFS_V2 in kernel.cpp
    assert SUPER_LBA + 1 + TABLE_SECT + DATA_SECTORS <= total_sectors, \
        "disk too small for MKFS data area"

    img = bytearray(total_sectors * 512)

    # Superblock (struct Superblock in kernel.cpp)
    sb = struct.pack('<4sHHIII',
                     b'MKFS',          # magic
                     MKFS_VERSION,                # version (must match kernel format)
                     0,                # file_count
                     DATA_LBA,         # data_start
                     DATA_LBA,         # free_lba
                     DATA_SECTORS,     # total_sectors
                     )
    img[SUPER_LBA * 512:SUPER_LBA * 512 + len(sb)] = sb
    # Table sectors stay zero (no files)

    out = os.path.join('build', 'data.vhd')
    os.makedirs('build', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(img)
    print("Wrote %s (%d MiB, %d sectors, MKFS data LBA %d-%d)"
          % (out, size_mb, total_sectors, DATA_LBA, DATA_LBA + DATA_SECTORS - 1))

if __name__ == '__main__':
    main()
