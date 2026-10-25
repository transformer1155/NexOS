#!/usr/bin/env python3
"""Parse the linux_sfs partition inside build/os_textboot.img and dump the
ELF header that the kernel's reader should produce for `linux_argv`."""
import struct, sys

IMG = r"D:\MyOS\bootloader\build\os_textboot.img"
SECTOR = 512
LINUX_SUPER_ONDISK_LBA = 12288   # SFS_LINUX_LBA
SFS_SUPER_LBA = 800
DELTA = LINUX_SUPER_ONDISK_LBA - SFS_SUPER_LBA   # 11488

def rd(img, lba, nsec=1):
    with open(img, "rb") as f:
        f.seek(lba * SECTOR)
        return f.read(nsec * SECTOR)

# superblock
sb = rd(IMG, LINUX_SUPER_ONDISK_LBA, 1)
magic = sb[0:4]
print(f"superblock LBA {LINUX_SUPER_ONDISK_LBA}: magic={magic}  (expect b'SFS\\x00')")
if magic != b"SFS\x00":
    print("  !! no SFS magic at 12288 -> linux_fs not mounted here")
    sys.exit(1)
data_start = struct.unpack_from("<I", sb, 8)[0]
print(f"  data_start(canonical)={data_start}  (expect 817)")

# directory: canonical 801..816 => on-disk 12289..12304
print("\n-- directory scan (on-disk LBA 12289+ --) --")
found = None
for s in range(16):
    sec = rd(IMG, LINUX_SUPER_ONDISK_LBA + 1 + s, 1)
    for e in range(16):
        off = e * 32
        name = sec[off:off+20].split(b"\x00")[0].decode("ascii", "replace")
        if not name:
            continue
        size = struct.unpack_from("<I", sec, off+20)[0]
        start_lba = struct.unpack_from("<I", sec, off+24)[0]
        ondisk = start_lba + DELTA
        tag = ""
        if name == "linux_argv":
            tag = "  <== TARGET"
            found = (name, size, start_lba, ondisk)
        print(f"  {name:20s} size={size:7d} start_lba(canon)={start_lba:6d} -> ondisk LBA {ondisk}{tag}")

if not found:
    print("\n!! linux_argv NOT found in linux_fs directory")
    sys.exit(1)

name, size, start_lba, ondisk = found
print(f"\n-- ELF header for {name} (read from on-disk LBA {ondisk}, {size} bytes) --")
data = rd(IMG, ondisk, (size + SECTOR - 1)//SECTOR)
print(f"  first 64 bytes hex: {data[:64].hex()}")
if data[:4] != b"\x7fELF":
    print("  !! NOT an ELF at computed location")
else:
    entry = struct.unpack_from("<I", data, 24)[0]
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsz = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    print(f"  e_entry   = 0x{entry:08X}")
    print(f"  e_phoff   = 0x{phoff:08X}")
    print(f"  e_phentsize = {phentsz}  (expect 32)")
    print(f"  e_phnum   = {phnum}  (expect 2)")
    if phnum > 0 and phnum <= 64 and phentsz > 0:
        print("  -> ELF header looks VALID; kernel should load fine.")
    else:
        print("  -> ELF header looks CORRUPT at this location (mismatch with reader).")
