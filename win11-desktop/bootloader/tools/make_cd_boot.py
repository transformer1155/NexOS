#!/usr/bin/env python3
"""
make_cd_boot.py - Create a CD boot image for El Torito no-emulation mode.

Layout (all 2048-byte aligned):
  CD sector 0:    boot_cd.bin (512B) + zero padding (1536B)
  CD sectors 1+:  kernel.bin (padded to 2048-byte boundary)

This image is used as the El Torito BIOS boot image in the ISO.
boot_cd.asm uses -boot-info-table to find this image's LBA on the CD,
then reads the kernel using INT 13h (2048-byte CD sectors).
"""
import sys, os

CD_SECTOR = 2048

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <boot_cd.bin> <kernel.bin> <output.img>")
        sys.exit(1)

    boot_bin = sys.argv[1]
    kernel_bin = sys.argv[2]
    out_img = sys.argv[3]

    boot_data = open(boot_bin, 'rb').read()
    kernel_data = open(kernel_bin, 'rb').read()

    if len(boot_data) != 512:
        print(f"ERROR: boot_cd.bin must be exactly 512 bytes, got {len(boot_data)}")
        sys.exit(1)

    # Pad boot sector to one CD sector (2048 bytes)
    boot_sector = boot_data + b'\x00' * (CD_SECTOR - len(boot_data))

    # Pad kernel to CD sector boundary
    kernel_sectors = (len(kernel_data) + CD_SECTOR - 1) // CD_SECTOR
    kernel_padded = kernel_data + b'\x00' * (kernel_sectors * CD_SECTOR - len(kernel_data))

    # Write the CD boot image
    with open(out_img, 'wb') as f:
        f.write(boot_sector)
        f.write(kernel_padded)

    total_sectors = 1 + kernel_sectors
    print(f"CD boot image: {out_img}")
    print(f"  Sector 0:      boot_cd.bin ({len(boot_data)}B + {CD_SECTOR - len(boot_data)}B padding)")
    print(f"  Sectors 1-{kernel_sectors}: kernel.bin ({len(kernel_data)}B, {kernel_sectors} CD sectors)")
    print(f"  Total: {total_sectors} CD sectors ({total_sectors * CD_SECTOR} bytes)")

if __name__ == '__main__':
    main()
