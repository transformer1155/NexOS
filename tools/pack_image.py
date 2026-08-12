#!/usr/bin/env python3
"""
pack_image.py - Inject your own applications into a NexOS disk image.

This script works on Windows, Linux and macOS using only the Python standard
library.  It rebuilds the Simple File System (SFS) payload from sfs_files/ plus
any files you supply, then writes the new SFS back into a bootable os.img at
LBA 3328 (byte offset 3328*512 = 1703936).

Usage examples:
  python tools/pack_image.py --list
  python tools/pack_image.py --add MyApp.exe MyData.txt --out myos.img
  python tools/pack_image.py --add ./dist/game.exe --img build/os.img --out gameos.img

The output image can be started with:
  make play IMG=myos.img      (if under WSL)
  qemu-system-x86_64 -m 128M -vga std -drive format=raw,file=myos.img
"""
import sys, os, struct, argparse, shutil

SECTOR = 512
ENTRY_SIZE = 32
ENTRIES_PER_SECTOR = SECTOR // ENTRY_SIZE
DIR_SECTORS = 16
DATA_START_LBA = 817           # Relative to the start of the SFS image
SFS_LBA = 3328                 # Where SFS is written on the BIOS disk
SFS_BYTE_OFF = SFS_LBA * SECTOR
MAX_NAME_LEN = 19              # name[20] with null terminator


def err(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sanitize_name(name):
    """Return a SFS-safe name or raise.  SFS names are ASCII, <=19 chars."""
    base = os.path.basename(name)
    try:
        base.encode('ascii')
    except UnicodeEncodeError:
        err(f"filename must be ASCII: {base}")
    if len(base) == 0:
        err("empty filename")
    if len(base) > MAX_NAME_LEN:
        err(f"filename too long ({len(base)} > {MAX_NAME_LEN} chars): {base}")
    return base


def collect_files(src_dir, extra_files):
    """Gather files from src_dir plus the extra user files, checking for name collisions."""
    files = []
    seen = set()

    if os.path.isdir(src_dir):
        for name in sorted(os.listdir(src_dir)):
            fpath = os.path.join(src_dir, name)
            if os.path.isfile(fpath):
                safe = sanitize_name(name)
                if safe in seen:
                    err(f"duplicate filename: {safe}")
                seen.add(safe)
                files.append((safe, open(fpath, 'rb').read()))

    for fpath in extra_files:
        if not os.path.isfile(fpath):
            err(f"file not found: {fpath}")
        safe = sanitize_name(fpath)
        if safe in seen:
            err(f"duplicate filename (shadows existing): {safe}")
        seen.add(safe)
        files.append((safe, open(fpath, 'rb').read()))

    if len(files) > 256:
        err(f"too many files: {len(files)} (SFS limit is 256)")
    return files


def build_sfs(files):
    """Build a binary SFS image identical to tools/sfs_gen.py."""
    entries = []
    current_lba = DATA_START_LBA
    file_data = b''

    for name, data in files:
        size = len(data)
        sectors = max(1, (size + SECTOR - 1) // SECTOR)
        padded = data + b'\x00' * (sectors * SECTOR - size)
        entries.append((name, size, current_lba))
        file_data += padded
        current_lba += sectors

    # Superblock
    sb = bytearray(SECTOR)
    sb[0:4] = b'SFS\x00'
    struct.pack_into('<H', sb, 4, 1)              # version
    struct.pack_into('<H', sb, 6, len(files))     # file_count
    struct.pack_into('<I', sb, 8, DATA_START_LBA) # data_start
    struct.pack_into('<I', sb, 12, current_lba)   # free_lba
    struct.pack_into('<I', sb, 16, 207)           # total_sectors

    # Directory
    directory = bytearray(DIR_SECTORS * SECTOR)
    for i, (name, size, lba) in enumerate(entries):
        offset = i * ENTRY_SIZE
        name_bytes = name.encode('ascii')
        directory[offset:offset + len(name_bytes)] = name_bytes
        directory[offset + len(name_bytes)] = 0
        struct.pack_into('<I', directory, offset + 20, size)
        struct.pack_into('<I', directory, offset + 24, lba)
        directory[offset + 28] = 0                  # type = file
        struct.pack_into('<H', directory, offset + 29, 0xFFFF)  # parent = root
        directory[offset + 31] = 0

    return bytes(sb) + bytes(directory) + file_data


def inject_sfs(base_img, sfs_blob, out_img):
    """Copy base_img to out_img and overwrite the SFS region with sfs_blob."""
    if not os.path.isfile(base_img):
        err(f"base image not found: {base_img}")
    shutil.copy(base_img, out_img)
    with open(out_img, 'r+b') as f:
        f.seek(SFS_BYTE_OFF)
        existing = f.read(len(sfs_blob))
        f.seek(SFS_BYTE_OFF)
        f.write(sfs_blob)
    return len(sfs_blob)


def list_sfs(img_path):
    """Print the directory of the SFS inside an existing disk image."""
    if not os.path.isfile(img_path):
        err(f"image not found: {img_path}")
    with open(img_path, 'rb') as f:
        f.seek(SFS_BYTE_OFF)
        sb = f.read(SECTOR)
    if sb[0:4] != b'SFS\x00':
        err(f"no SFS signature at LBA {SFS_LBA}")
    count = struct.unpack_from('<H', sb, 6)[0]
    data_start = struct.unpack_from('<I', sb, 8)[0]
    print(f"SFS at LBA {SFS_LBA} ({count} files, data LBA {data_start}):")
    with open(img_path, 'rb') as f:
        f.seek(SFS_BYTE_OFF + SECTOR)
        directory = f.read(DIR_SECTORS * SECTOR)
    for i in range(count):
        off = i * ENTRY_SIZE
        name = directory[off:off + 20].split(b'\x00', 1)[0].decode('ascii', 'replace')
        size = struct.unpack_from('<I', directory, off + 20)[0]
        lba = struct.unpack_from('<I', directory, off + 24)[0]
        print(f"  {name:<20} {size:>8} bytes  LBA {lba}")


def main():
    parser = argparse.ArgumentParser(
        description="Inject applications into a NexOS disk image.")
    parser.add_argument('--img', default='build/os.img',
                        help='source disk image (default: build/os.img)')
    parser.add_argument('--out', default=None,
                        help='output disk image (default: overwrite --img)')
    parser.add_argument('--add', nargs='*', default=[],
                        help='files to add/replace in the SFS')
    parser.add_argument('--sfs-dir', default='sfs_files',
                        help='directory containing the base SFS files')
    parser.add_argument('--list', action='store_true',
                        help='list current SFS contents and exit')
    args = parser.parse_args()

    if args.list:
        list_sfs(args.img)
        return 0

    out_img = args.out if args.out else args.img
    files = collect_files(args.sfs_dir, args.add)

    print(f"Packing {len(files)} files into SFS...")
    for name, data in files:
        print(f"  {name}: {len(data)} bytes")

    sfs_blob = build_sfs(files)
    written = inject_sfs(args.img, sfs_blob, out_img)

    print(f"\nWrote SFS ({written} bytes) at byte {SFS_BYTE_OFF} of {out_img}")
    print(f"Image size: {os.path.getsize(out_img)} bytes")
    print("Done.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
