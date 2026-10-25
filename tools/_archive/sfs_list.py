#!/usr/bin/env python3
"""Host-side parser for the NexOS SFS image (build/sfs.img).

Layout (matching .attic64/kernel64.cpp):
  sector 0  : superblock  (magic "SFS\0", version, file_count, ...)
  sector 1..: directory   (SFS_DIR_SECT=16 sectors, 16 entries/sector,
                            FS_ENTRY_SIZE=32 bytes each)
  entry     : name[20] + size(u32) + start_lba(u32) + type(u8) + parent(u16) + reserved(u8)
"""
import sys, struct

FS_NAME_LEN = 20
FS_ENTRY_SIZE = 32
FS_ENTRY_PER_SEC = 16
SFS_DIR_SECT = 16

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/sfs.img"
    with open(path, "rb") as f:
        data = f.read()

    sb = data[0:512]
    magic = sb[0:4]
    version, file_count = struct.unpack_from("<HH", sb, 4)
    data_start, free_lba, total_sectors = struct.unpack_from("<III", sb, 8)
    print(f"superblock: magic={magic!r} version={version} file_count={file_count} "
          f"data_start={data_start} free_lba={free_lba} total_sectors={total_sectors}")

    if magic != b"SFS\x00":
        print("WARNING: not a valid SFS image (bad magic)")
        return

    print(f"\nDirectory entries ({SFS_DIR_SECT} sectors x {FS_ENTRY_PER_SEC} = "
          f"{SFS_DIR_SECT*FS_ENTRY_PER_SEC} slots):")
    found = []
    for s in range(SFS_DIR_SECT):
        sec_off = (1 + s) * 512
        for e in range(FS_ENTRY_PER_SEC):
            ent_off = sec_off + e * FS_ENTRY_SIZE
            ent = data[ent_off:ent_off + FS_ENTRY_SIZE]
            if len(ent) < FS_ENTRY_SIZE:
                break
            name = ent[0:FS_NAME_LEN].split(b"\x00", 1)[0].decode("latin1", "replace")
            if ent[0] == 0:
                continue
            size, start_lba = struct.unpack_from("<II", ent, 20)
            typ = ent[28]
            parent = struct.unpack_from("<H", ent, 29)[0]
            print(f"  [{s*FS_ENTRY_PER_SEC+e:3d}] name={name!r:24} size={size:8d} "
                  f"start_lba={start_lba:6d} type={typ} parent={parent}")
            found.append(name)

    print(f"\nTotal files listed: {len(found)}")
    target = "msyh.ttf"
    if target in found:
        idx = found.index(target)
        print(f"*** FOUND {target!r} at slot {idx} ***")
    else:
        print(f"*** {target!r} NOT FOUND in image ***")
        # fuzzy match
        for n in found:
            if "msyh" in n.lower() or "ttf" in n.lower():
                print(f"    (similar: {n!r})")

if __name__ == "__main__":
    main()
