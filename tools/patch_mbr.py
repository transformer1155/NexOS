#!/usr/bin/env python3
"""
patch_mbr.py - Patch MBR partition table into a disk image.

Adds a valid MBR partition table entry to a disk image so that El Torito
hard-disk-emulation BIOS boot works reliably on real hardware.

The partition table is written at offset 446 (first entry) without
modifying the boot code (which occupies the first ~150 bytes).
"""
import struct, sys

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_img> <output_img>")
        sys.exit(1)

    inp, outp = sys.argv[1], sys.argv[2]
    data = bytearray(open(inp, 'rb').read())

    total_sectors = len(data) // 512
    if total_sectors < 2:
        print("ERROR: image too small")
        sys.exit(1)

    # Build partition table entry #1 (16 bytes at offset 446)
    part = bytearray(16)
    part[0] = 0x80        # bootable flag
    # Start CHS: head=0, sector=1, cylinder=0
    part[1] = 0x00        # head
    part[2] = 0x01        # sector(1) | cyl_high(0)
    part[3] = 0x00        # cyl_low
    part[4] = 0x83        # type = Linux (any type works for geometry detection)
    # End CHS: head=254, sector=63, cylinder=1023 (standard max-CHS)
    part[5] = 0xFE        # head
    part[6] = 0xFF        # sector(63=0x3F) | cyl_high(3=0xC0) => 0xFF
    part[7] = 0xFF        # cyl_low(255)
    struct.pack_into('<I', part, 8, 1)                   # start LBA
    struct.pack_into('<I', part, 12, total_sectors - 1)  # num sectors

    data[446:462] = part

    open(outp, 'wb').write(data)
    print(f"Patched MBR: {inp} -> {outp} ({total_sectors} sectors, "
          f"partition 1: LBA 1, {total_sectors-1} sectors)")

if __name__ == '__main__':
    main()
