#!/usr/bin/env python3
"""
make_gpt_uefi.py - Build a GPT disk image with ESP partition for UEFI boot.

The disk layout (128 MB total):
  LBA 0:      Protective MBR
  LBA 1:      GPT header
  LBA 2-33:   GPT partition entry array
  LBA 34-399: Unused (gap before kernel64 LBA)
  LBA 400:    kernel64.bin (written by caller, raw disk offset)
  LBA 800:    SFS image (written by caller, raw disk offset)
  LBA 1024+:  ESP partition (FAT32, contains BOOTX64.EFI)

Usage:
    python3 make_gpt_uefi.py <esp_img> <output> [kernel64] [sfs]
      <esp_img>  - FAT32 image containing BOOTX64.EFI at /EFI/BOOT/BOOTX64.EFI
      <output>   - Output GPT disk image
      <kernel64> - Optional: kernel64.bin to write at LBA 400
      <sfs>      - Optional: SFS image to write at LBA 800
"""

import struct
import sys
import os

# Constants
SECTOR_SIZE = 512
GPT_ENTRY_SIZE = 128
GPT_ENTRIES = 32
ESP_GUID = b'\x28\x73\x2a\xc1\x1f\xf8\xd2\x11\xba\x4b\x00\xa0\xc9\x3e\xc9\x3b'  # C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PART_GUID = b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'  # unused

# Disk layout
DISK_SECTORS = 262144  # 128 MB
ESP_LBA_START = 1024   # ESP starts at LBA 1024 (after kernel64 at 400, SFS at 800)
KERNEL64_LBA = 400
SFS_LBA = 800


def create_protective_mbr():
    """Create a protective MBR for GPT."""
    mbr = bytearray(SECTOR_SIZE)
    # Partition table entry 1: protective MBR
    # LBA 0 is the MBR itself
    # Partition covers the whole disk (type 0xEE)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    # Partition entry 1 at offset 446
    # Status: 0x00 (not bootable, protective)
    mbr[446] = 0x00  # status
    # CHS first sector: 0x00, 0x02, 0x00 (simplified)
    mbr[447] = 0x00  # head
    mbr[448] = 0x02  # sector/cylinder
    mbr[449] = 0x00  # cylinder
    mbr[450] = 0xEE  # partition type (GPT protective)
    # CHS last sector: 0xFF, 0xFF, 0xFF
    mbr[451] = 0xFF  # head
    mbr[452] = 0xFF  # sector/cylinder
    mbr[453] = 0xFF  # cylinder
    # LBA start (LBA 1, since partition type 0xEE traditionally starts at 1)
    struct.pack_into('<I', mbr, 454, 1)
    # LBA size (remaining disk sectors)
    struct.pack_into('<I', mbr, 458, DISK_SECTORS - 1)
    return bytes(mbr)


def create_gpt_header(partitions_lba, parts_bytes):
    """Create GPT header."""
    # Compute CRC32 of partition entries
    crc_table = _crc32_table()
    parts_crc = _crc32(crc_table, parts_bytes)

    hdr = bytearray(SECTOR_SIZE)
    # Signature
    hdr[0:8] = b'EFI PART'
    # Revision 1.0
    struct.pack_into('<I', hdr, 8, 0x00010000)
    # Header size
    struct.pack_into('<I', hdr, 12, 92)
    # CRC32 of header (will be filled last)
    # Reserved
    # My LBA (this header is at LBA 1)
    struct.pack_into('<Q', hdr, 24, 1)
    # Backup LBA (last LBA of disk)
    struct.pack_into('<Q', hdr, 32, DISK_SECTORS - 1)
    # First usable LBA
    struct.pack_into('<Q', hdr, 40, partitions_lba + GPT_ENTRIES)
    # Last usable LBA
    struct.pack_into('<Q', hdr, 48, DISK_SECTORS - 1 - 1)  # leave room for backup header
    # Disk GUID (random but fixed for reproducibility)
    hdr[56:72] = b'\x00' * 16  # simplified, use zeros
    # Partition entry LBA
    struct.pack_into('<Q', hdr, 72, partitions_lba)
    # Number of partition entries
    struct.pack_into('<I', hdr, 80, GPT_ENTRIES)
    # Size of partition entry
    struct.pack_into('<I', hdr, 84, GPT_ENTRY_SIZE)
    # CRC32 of partition entries
    struct.pack_into('<I', hdr, 88, parts_crc)

    # Compute CRC32 of header (with parts_crc filled, but crc field set to 0)
    hdr_crc = _crc32(crc_table, bytes(hdr[:92]))
    struct.pack_into('<I', hdr, 16, hdr_crc)

    return bytes(hdr)


def create_gpt_partitions(esp_lba_start, esp_lba_size):
    """Create GPT partition entry array."""
    entries = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)

    # Partition 1: EFI System Partition
    off = 0
    # Partition type GUID (EFI System Partition)
    entries[off:off+16] = ESP_GUID
    # Unique partition GUID
    unique_guid = b'\x00' * 16  # simplified
    entries[off+16:off+32] = unique_guid
    # Starting LBA
    struct.pack_into('<Q', entries, off + 32, esp_lba_start)
    # Ending LBA
    struct.pack_into('<Q', entries, off + 40, esp_lba_start + esp_lba_size - 1)
    # Attributes
    struct.pack_into('<Q', entries, off + 48, 0)
    # Name (UTF-16LE, 36 bytes max = 18 characters)
    name = 'EFI System Partition'
    for i, ch in enumerate(name):
        struct.pack_into('<H', entries, off + 56 + i * 2, ord(ch))

    return bytes(entries)


def _crc32_table():
    """Build CRC32 lookup table."""
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        table.append(crc)
    return table


def _crc32(table, data):
    """Compute CRC32 of data."""
    crc = 0xFFFFFFFF
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <esp_img> <output> [kernel64] [sfs]")
        sys.exit(1)

    esp_path = sys.argv[1]
    out_path = sys.argv[2]
    kernel64_path = sys.argv[3] if len(sys.argv) > 3 else None
    sfs_path = sys.argv[4] if len(sys.argv) > 4 else None

    if not os.path.exists(esp_path):
        print(f"Error: ESP image not found: {esp_path}")
        sys.exit(1)

    # Read ESP image (FAT32 filesystem)
    with open(esp_path, 'rb') as f:
        esp_data = f.read()

    # Calculate ESP partition size from the actual image file
    esp_lba_size = (len(esp_data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    # Round up to nearest 8 (alignment for flash media)
    esp_lba_size = (esp_lba_size + 7) & ~7
    print(f"[GPT] ESP image: {len(esp_data)} bytes = {esp_lba_size} sectors")

    # Create disk image
    disk_size = DISK_SECTORS * SECTOR_SIZE
    disk = bytearray(disk_size)

    # Write protective MBR at LBA 0
    mbr = create_protective_mbr()
    disk[0:SECTOR_SIZE] = mbr
    print(f"[GPT] Protective MBR written at LBA 0")

    # Write GPT partition entries at LBA 2
    parts_lba = 2
    parts = create_gpt_partitions(ESP_LBA_START, esp_lba_size)
    disk[parts_lba * SECTOR_SIZE:(parts_lba + GPT_ENTRIES) * SECTOR_SIZE] = parts
    print(f"[GPT] Partition entries at LBA {parts_lba}")

    # Write GPT header at LBA 1
    gpt = create_gpt_header(parts_lba, parts)
    disk[SECTOR_SIZE:2 * SECTOR_SIZE] = gpt
    print(f"[GPT] GPT header at LBA 1")

    # Write ESP partition (FAT32 filesystem) at LBA ESP_LBA_START
    esp_offset = ESP_LBA_START * SECTOR_SIZE
    esp_size = esp_lba_size * SECTOR_SIZE
    if len(esp_data) > esp_size:
        print(f"Warning: ESP image ({len(esp_data)} bytes) larger than partition ({esp_size} bytes)")
        esp_data = esp_data[:esp_size]
    disk[esp_offset:esp_offset + len(esp_data)] = esp_data
    print(f"[GPT] ESP partition at LBA {ESP_LBA_START}-{ESP_LBA_START + esp_lba_size - 1} "
          f"({esp_lba_size} sectors, {len(esp_data)} bytes)")

    # Write backup GPT at end of disk
    backup_parts_lba = DISK_SECTORS - 1 - GPT_ENTRIES
    disk[backup_parts_lba * SECTOR_SIZE:][:len(parts)] = parts
    # Backup header
    backup_hdr = bytearray(gpt)
    struct.pack_into('<Q', backup_hdr, 24, DISK_SECTORS - 1)  # MyLBA = last sector
    struct.pack_into('<Q', backup_hdr, 32, 1)  # Backup LBA = 1
    # Recompute CRC
    crc_table = _crc32_table()
    hdr_crc = _crc32(crc_table, bytes(backup_hdr[:92]))
    struct.pack_into('<I', backup_hdr, 16, hdr_crc)
    disk[(DISK_SECTORS - 1) * SECTOR_SIZE:] = bytes(backup_hdr)

    # Write disk
    with open(out_path, 'wb') as f:
        f.write(disk)

    print(f"[GPT] Disk image written: {out_path} ({disk_size} bytes)")

    # Write kernel64.bin at LBA 400 if provided
    if kernel64_path and os.path.exists(kernel64_path):
        with open(kernel64_path, 'rb') as f:
            k64 = f.read()
        k64_offset = KERNEL64_LBA * SECTOR_SIZE
        k64_sectors = (len(k64) + SECTOR_SIZE - 1) // SECTOR_SIZE
        if k64_offset + len(k64) <= disk_size:
            with open(out_path, 'r+b') as f:
                f.seek(k64_offset)
                f.write(k64)
            print(f"[GPT] kernel64.bin at LBA {KERNEL64_LBA} ({k64_sectors} sectors, {len(k64)} bytes)")
        else:
            print(f"Warning: kernel64.bin too large for disk")

    # Write SFS image at LBA 800 if provided
    if sfs_path and os.path.exists(sfs_path):
        with open(sfs_path, 'rb') as f:
            sfs = f.read()
        sfs_offset = SFS_LBA * SECTOR_SIZE
        sfs_sectors = (len(sfs) + SECTOR_SIZE - 1) // SECTOR_SIZE
        if sfs_offset + len(sfs) <= disk_size:
            with open(out_path, 'r+b') as f:
                f.seek(sfs_offset)
                f.write(sfs)
            print(f"[GPT] SFS image at LBA {SFS_LBA} ({sfs_sectors} sectors, {len(sfs)} bytes)")
        else:
            print(f"Warning: SFS image too large for disk")

    print(f"[GPT] Done!")


if __name__ == '__main__':
    main()