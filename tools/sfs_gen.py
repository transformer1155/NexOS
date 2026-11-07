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

# --- known long names, PER VOLUME --------------------------------------------
# The same relative path means different things in each volume, so the tolerance
# list must be selected by WHICH volume is being packed -- never by guessing at a
# path prefix.
#
# sfs_files volume: the kernel reads these, and the 32-bit kernel already adapts by
# truncating its lookup name too (kernel.cpp cmd_skill caps at FS_NAME_LEN-1).  So
# they are REACHABLE and their names are load-bearing -- renaming would break
# `skill view app_calculator`.
KNOWN_TRUNCATED_SFS_FILES = {
    'msyh_regular_backup.ttf',
    'src_app_browser.skill',
    'src_app_calculator.skill',
    'src_app_control_panel.skill',
    'src_app_file_explorer.skill',
    'src_app_task_manager.skill',
    'src_app_terminal.skill',
    'src_font_bitmap.skill',
    'src_font_vector.skill',
    'src_input_keyboard.skill',
    'src_input_mouse.skill',
    'src_theme_default.skill',
}

# linux_root volume: a DIFFERENT category, kept separate so the two cannot be
# confused.  No by-name reader exists anywhere in the tree for these (grep for
# alpine-initramfs / minirootfs / mkinitramfs: 0 hits outside linux_root), so they
# are UNRESOLVED rather than 'compensated by a reader'.  Whether to clean them up or
# keep them stays an open decision, because a rename could break a launcher or
# external script we never scanned.
KNOWN_TRUNCATED_LINUX = {
    'initrd/alpine-initramfs.cpio.gz',
    'initrd/minirootfs.tar.gz',
    'initrd/mkinitramfs.py',
}


def _is_linux_volume(src_dir):
    """True when packing the Linux user-space volume.

    Decided by the volume's own name rather than by matching a path substring: a
    prefix guess would silently pick the wrong set if the layout ever moves.
    """
    return os.path.basename(os.path.abspath(src_dir)) == 'linux_root'


def main():
    if len(sys.argv) < 3 or sys.argv[1].startswith('-'):
        print(f"Usage: {sys.argv[0]} <source_dir> <output_img> [--exclude-tex] [--exclude-fonts]")
        print(f"  Note: this tool PACKS files; it cannot list an existing image.")
        sys.exit(1)

    src_dir = sys.argv[1]
    out_img = sys.argv[2]
    exclude_tex = '--exclude-tex' in sys.argv[3:]
    exclude_fonts = '--exclude-fonts' in sys.argv[3:]

    if not os.path.isdir(src_dir):
        print(f"Error: source directory '{src_dir}' does not exist")
        sys.exit(1)

    # Collect files from source directory, recursing into subdirectories.
    # BUG-N6 fix: this used os.listdir() + isfile(), which silently DROPPED
    # every subdirectory -- so sfs_files/lang/{en,zh}.txt never made it into
    # the image and the managed shell logged "[SFS-FIND] not found:
    # lang/zh.txt" (Lang.cs loads its pack via Host.ReadText(1,"lang/zh.txt")).
    # Sfs::find() matches entry names with a flat strcmp and has no directory
    # support, so a nested file is stored under its relative path
    # ("lang/zh.txt") as a single flat entry name -- which is exactly the
    # string the caller asks for. The name field is 20 bytes, so the path must
    # stay within 19 characters.
    files = []
    if os.path.isdir(src_dir):
        for root, dirs, names in os.walk(src_dir):
            dirs.sort()
            rel_root = os.path.relpath(root, src_dir)
            for name in sorted(names):
                fpath = os.path.join(root, name)
                if not os.path.isfile(fpath):
                    continue
                # Relative, forward-slash entry name ("" for the root itself).
                rel = name if rel_root == '.' else os.path.join(rel_root, name)
                rel = rel.replace(os.sep, '/')
                if exclude_tex and name.startswith('tex_'):
                    continue
                if exclude_fonts and name in ('zfont.bin', 'afont.bin'):
                    continue
                data = open(fpath, 'rb').read()
                if len(rel) >= 20:
                    # The SFS name field is 20 bytes (19 chars + NUL, see
                    # kernel64.cpp FS_NAME_LEN).  The READER compares full names
                    # exactly, so a truncated name can never be opened by its
                    # real path -- it is silently unaddressable.
                    #
                    # KNOWN_TRUNCATED lists the entries that were already over
                    # the limit before this check existed.  They CANNOT simply be
                    # renamed: the 32-bit kernel already adapts by truncating its
                    # lookup name too (kernel.cpp cmd_skill caps at FS_NAME_LEN-1),
                    # so those files are reachable and their names are load-bearing.
                    # Renaming them would break `skill view app_calculator`.  They
                    # are reported explicitly, not as a vague warning.
                    #
                    # Anything NEW that exceeds the limit is a hard error: that is
                    # the whole point -- it turns a silent failure into a build-time
                    # one.  Do NOT widen this set to silence a real problem; instead
                    # shorten the name (e.g. coop-bugfix.json rather than
                    # ai/cooperation/bugfix-end-to-end.json).
                    # Tolerance list chosen by the volume being packed (see the
                    # KNOWN_TRUNCATED_* sets at module scope).
                    allow = (KNOWN_TRUNCATED_LINUX if _is_linux_volume(src_dir)
                             else KNOWN_TRUNCATED_SFS_FILES)
                    if rel not in allow:
                        sys.exit(f"ERROR: SFS name field is 20 bytes (19 chars); "
                                 f"'{rel}' ({len(rel)} chars) would be silently truncated "
                                 "and could never be opened by its full name. "
                                 "Shorten it, or add a justified entry to the right "
                                 "volume's known-long-name set.")
                    print(f"note: '{rel}' is a known long name in this volume "
                          f"(tolerated -- see the set for why)")
                    rel = rel[:19]
                files.append((rel, data))

    print(f"SFS: packing {len(files)} files from {src_dir}")

    # Build the image
    SECTOR = 512
    ENTRY_SIZE = 32
    ENTRIES_PER_SECTOR = SECTOR // ENTRY_SIZE  # 16
    DIR_SECTORS = 16
    SFS_SUPER_LBA = 800
    DATA_START_LBA = SFS_SUPER_LBA + DIR_SECTORS + 1  # super + dir = 817

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
    struct.pack_into('<I', sb, 16, current_lba - SFS_SUPER_LBA)  # total_sectors (metadata only)

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
