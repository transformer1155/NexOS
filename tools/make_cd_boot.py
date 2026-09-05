#!/usr/bin/env python3
"""
make_cd_boot.py - Create a CD boot image for El Torito no-emulation mode.

Layout (all 2048-byte aligned):
  CD sector 0:    boot_cd.bin (padded to 2048B)
  CD sectors 1+:  kernel.bin (padded to 2048-byte boundary)
  CD sectors 1+kernel_sectors+:  [optional] texture-free SFS image
                                  (padded to 2048-byte boundary)

This image is used as the El Torito BIOS boot image in the ISO.
boot_cd.asm uses -boot-info-table to find this image's LBA on the CD,
then reads the kernel and (optionally) the SFS image using INT 13h
(2048-byte CD sectors).  The SFS image is streamed into high RAM so the
32-bit kernel can mount it on a CD boot (where no ATA disk exists).

The kernel/SFS CD-sector counts are patched into boot_cd.bin at file
offsets 0x1E4 / 0x1E0 respectively so the loader knows where each blob
begins on the CD.
"""
import sys, os, struct

CD_SECTOR = 2048

# boot_cd.bin patch offsets (file bytes) -- tail of the 2048-byte boot sector
OFF_KERNEL_CD_SECTS = 0x7F4
OFF_SFS_CD_SECTS    = 0x7F0

def main():
    if len(sys.argv) not in (4, 5):
        print(f"Usage: {sys.argv[0]} <boot_cd.bin> <kernel.bin> <output.img> [sfs_cd.img]")
        sys.exit(1)

    boot_bin = sys.argv[1]
    kernel_bin = sys.argv[2]
    out_img = sys.argv[3]
    sfs_bin = sys.argv[4] if len(sys.argv) == 5 else None

    boot_data = open(boot_bin, 'rb').read()
    kernel_data = open(kernel_bin, 'rb').read()

    if len(boot_data) < 512 or len(boot_data) > CD_SECTOR:
        print(f"ERROR: boot_cd.bin must be 512..{CD_SECTOR} bytes, got {len(boot_data)}")
        sys.exit(1)

    # Pad boot sector to one CD sector (2048 bytes)
    boot_sector = boot_data + b'\x00' * (CD_SECTOR - len(boot_data))

    # Pad kernel to CD sector boundary
    kernel_sectors = (len(kernel_data) + CD_SECTOR - 1) // CD_SECTOR
    kernel_padded = kernel_data + b'\x00' * (kernel_sectors * CD_SECTOR - len(kernel_data))

    # Patch the kernel/SFS CD-sector counts into boot_cd.bin before assembling
    # the image (the loader reads them at 0x1E4 / 0x1E0).
    boot_data = bytearray(boot_data)
    struct.pack_into('<I', boot_data, OFF_KERNEL_CD_SECTS, kernel_sectors)
    sfs_sectors = 0
    if sfs_bin is not None:
        sfs_data = open(sfs_bin, 'rb').read()
        sfs_sectors = (len(sfs_data) + CD_SECTOR - 1) // CD_SECTOR
        sfs_padded = sfs_data + b'\x00' * (sfs_sectors * CD_SECTOR - len(sfs_data))
        struct.pack_into('<I', boot_data, OFF_SFS_CD_SECTS, sfs_sectors)
    # re-pad boot sector after patching
    boot_sector = bytes(boot_data) + b'\x00' * (CD_SECTOR - len(boot_data))

    # Write the CD boot image
    with open(out_img, 'wb') as f:
        f.write(boot_sector)
        f.write(kernel_padded)
        if sfs_bin is not None:
            f.write(sfs_padded)

    total_sectors = 1 + kernel_sectors + sfs_sectors
    print(f"CD boot image: {out_img}")
    print(f"  Sector 0:      boot_cd.bin ({len(boot_data)}B + {CD_SECTOR - len(boot_data)}B padding)")
    print(f"  Sectors 1-{kernel_sectors}: kernel.bin ({len(kernel_data)}B, {kernel_sectors} CD sectors)")
    if sfs_bin is not None:
        print(f"  Sectors {1+kernel_sectors}-{1+kernel_sectors+sfs_sectors-1}: sfs_cd.img ({len(sfs_data)}B, {sfs_sectors} CD sectors)")
    print(f"  Total: {total_sectors} CD sectors ({total_sectors * CD_SECTOR} bytes)")

if __name__ == '__main__':
    main()
