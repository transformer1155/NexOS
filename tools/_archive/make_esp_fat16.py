#!/usr/bin/env python3
"""
make_esp_fat16.py - Build a minimal FAT16 ESP image containing a single
EFI application at \\EFI\\BOOT\\BOOTX64.EFI, WITHOUT mtools.

This is used to assemble a UEFI-bootable ESP when mtools is unavailable
(e.g. pure-Windows MSYS2 builds).  OVMF's FAT driver reads the resulting
FAT16 volume and finds the fallback bootloader at \\EFI\\BOOT\\BOOTX64.EFI.

Usage:
    python3 make_esp_fat16.py <bootx64.efi> <output.img> [size_mb]
"""
import struct
import sys
import os

SECTOR = 512


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def short_name(name):
    """Convert a filename like 'BOOTX64.EFI' to (8-byte name, 3-byte ext)."""
    base, dot, ext = name.partition('.')
    base = base.upper()[:8].ljust(8, ' ')
    ext = ext.upper()[:3].ljust(3, ' ')
    return base.encode('ascii'), ext.encode('ascii')


def dir_entry(name, attr, first_cluster, size):
    # FAT16 32-byte directory entry:
    #   0:8  name, 8:11 ext, 11 attr, 12 NTRes, 13 crt-time-tenth,
    #   14:2 crt time, 16:2 crt date, 18:2 last-access date,
    #   20:2 first cluster HIGH (0 for FAT16), 22:2 write time, 24:2 write date,
    #   26:2 first cluster LOW, 28:4 file size
    base, ext = short_name(name)
    e = bytearray(32)
    e[0:8] = base
    e[8:11] = ext
    e[11] = attr
    struct.pack_into('<H', e, 20, 0)        # first cluster high (FAT16 = 0)
    struct.pack_into('<H', e, 26, first_cluster & 0xFFFF)  # first cluster low
    struct.pack_into('<I', e, 28, size & 0xFFFFFFFF)       # file size
    return bytes(e)


def build(fw_path, out_path, size_mb=16):
    fw = open(fw_path, 'rb').read()
    total_sectors = size_mb * 1024 * 1024 // SECTOR      # 16 MiB -> 32768
    sec_per_cluster = 1
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_sectors = align_up(root_entries * 32, SECTOR) // SECTOR  # 32

    # Compute FAT size for FAT16 (max 65524 clusters)
    # data_sectors = total - reserved - num_fats*fat_sec - root_sectors
    # clusters = data_sectors / sec_per_cluster
    # fat_sec = ceil((clusters+2)*2 / SECTOR)
    # Solve by iteration:
    fat_sec = 128
    for _ in range(8):
        data_sectors = total_sectors - reserved - num_fats * fat_sec - root_sectors
        clusters = data_sectors // sec_per_cluster
        need = align_up((clusters + 2) * 2, SECTOR) // SECTOR
        if need == fat_sec:
            break
        fat_sec = need
    data_sectors = total_sectors - reserved - num_fats * fat_sec - root_sectors
    clusters = data_sectors // sec_per_cluster

    img = bytearray(total_sectors * SECTOR)

    # ---- Boot sector (BPB) ----
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x3C\x90'        # x86 jump
    bs[3:11] = b'NEXOSESP'          # OEM
    struct.pack_into('<H', bs, 11, SECTOR)
    bs[13] = sec_per_cluster
    struct.pack_into('<H', bs, 14, reserved)
    bs[16] = num_fats
    struct.pack_into('<H', bs, 17, root_entries)
    # total sectors 16-bit (valid because < 65536)
    struct.pack_into('<H', bs, 19, total_sectors)
    bs[21] = 0xF8                    # media descriptor
    struct.pack_into('<H', bs, 22, fat_sec)
    struct.pack_into('<H', bs, 24, 32)   # sec per track (dummy)
    struct.pack_into('<H', bs, 26, 64)   # num heads (dummy)
    struct.pack_into('<I', bs, 28, 0)    # hidden sectors
    # FAT16 extended BPB
    bs[36] = 0x00                    # drive number
    bs[37] = 0x00                    # reserved
    bs[38] = 0x29                    # boot signature
    struct.pack_into('<I', bs, 39, 0x12345678)  # volume serial
    bs[43:54] = b'NEXOSESP   '      # volume label (11)
    bs[54:59] = b'FAT16'             # fs type
    bs[510] = 0x55
    bs[511] = 0xAA
    img[0:SECTOR] = bs

    # ---- FAT region ----
    fat_start = reserved
    # Build FAT: cluster 0/1 reserved (0xFFF8 media + 0xFFFF), then chains.
    fat = bytearray(fat_sec * SECTOR)
    struct.pack_into('<H', fat, 0, 0xFFF8)   # media
    struct.pack_into('<H', fat, 2, 0xFFFF)   # EOC for cluster 1
    # We'll fill cluster chains below; mark EOC for used clusters.
    # cluster numbering: cluster 2 == first data sector.

    def set_fat(cluster, value):
        off = cluster * 2
        if off + 2 <= len(fat):
            struct.pack_into('<H', fat, off, value & 0xFFFF)

    # Layout (data clusters start at cluster 2):
    #   cluster 2 : EFI\ directory
    #   cluster 3 : EFI\BOOT\ directory
    #   cluster 4..: BOOTX64.EFI file data
    file_clusters = (len(fw) + SECTOR - 1) // SECTOR
    # data region offset in sectors
    data_start = reserved + num_fats * fat_sec + root_sectors

    # Root directory sector offset
    root_start = reserved + num_fats * fat_sec
    # Write root dir: "EFI" entry
    root = bytearray(root_sectors * SECTOR)
    root[0:32] = dir_entry('EFI', 0x10, 2, 0)   # directory, cluster 2

    # startup.nsh - OVMF drops to the EFI Shell for a fixed HDD (no NVRAM
    # boot entry), but the shell auto-runs startup.nsh from the ESP.  Use it
    # to launch our fallback bootloader.  This avoids needing a removable
    # device or a persisted Boot#### variable.
    nsh = b"FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n"
    nsh_clusters = (len(nsh) + SECTOR - 1) // SECTOR
    nsh_start = 4 + file_clusters
    for i in range(nsh_clusters):
        c = nsh_start + i
        nxt = nsh_start + i + 1
        set_fat(c, 0xFFFF if i == nsh_clusters - 1 else nxt)
    root[32:64] = dir_entry('STARTUP.NSH', 0x20, nsh_start, len(nsh))
    nsh_padded = nsh + b'\x00' * ((nsh_clusters * SECTOR) - len(nsh))
    off = (data_start + (nsh_start - 2)) * SECTOR
    img[off:off + len(nsh_padded)] = nsh_padded

    # EFI dir content (cluster 2): "BOOT" subdir
    efi_dir = bytearray(SECfor_cluster := sec_per_cluster * SECTOR)
    efi_dir[0:32] = dir_entry('BOOT', 0x10, 3, 0)
    # dot entries
    efi_dir[32:64] = dir_entry('.', 0x10, 2, 0)
    efi_dir[64:96] = dir_entry('..', 0x10, 0, 0)

    # BOOT dir content (cluster 3): "BOOTX64.EFI" file
    boot_dir = bytearray(SECfor_cluster)
    boot_dir[0:32] = dir_entry('BOOTX64.EFI', 0x20, 4, len(fw))
    boot_dir[32:64] = dir_entry('.', 0x10, 3, 0)
    boot_dir[64:96] = dir_entry('..', 0x10, 2, 0)

    # File data clusters (cluster 4 .. 4+file_clusters-1)
    fw_padded = fw + b'\x00' * ((file_clusters * SECTOR) - len(fw))
    # chain
    for i in range(file_clusters):
        c = 4 + i
        nxt = 4 + i + 1
        set_fat(c, 0xFFFF if i == file_clusters - 1 else nxt)

    # Single-cluster directories also need an EOC marker in the FAT,
    # otherwise the FAT driver treats their clusters as free/unallocated.
    set_fat(2, 0xFFFF)   # EFI\      directory
    set_fat(3, 0xFFFF)   # EFI\BOOT\ directory

    # Place FAT copies
    for f in range(num_fats):
        off = (fat_start + f * fat_sec) * SECTOR
        img[off:off + len(fat)] = fat

    # Place root dir
    off = root_start * SECTOR
    img[off:off + len(root)] = root

    # Place EFI dir (cluster 2)
    off = (data_start + (2 - 2)) * SECTOR
    img[off:off + len(efi_dir)] = efi_dir

    # Place BOOT dir (cluster 3)
    off = (data_start + (3 - 2)) * SECTOR
    img[off:off + len(boot_dir)] = boot_dir

    # Place file data (cluster 4+)
    off = (data_start + (4 - 2)) * SECTOR
    img[off:off + len(fw_padded)] = fw_padded

    with open(out_path, 'wb') as f:
        f.write(img)

    print(f"[ESP] FAT16 image: {len(img)} bytes ({total_sectors} sectors)")
    print(f"[ESP] FAT sectors={fat_sec} clusters={clusters} file_clusters={file_clusters}")
    print(f"[ESP] BOOTX64.EFI: {len(fw)} bytes at cluster 4")
    print(f"[ESP] Wrote {out_path}")


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <bootx64.efi> <output.img> [size_mb]")
        sys.exit(1)
    fw = sys.argv[1]
    out = sys.argv[2]
    sz = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    build(fw, out, sz)
