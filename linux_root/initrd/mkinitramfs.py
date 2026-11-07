#!/usr/bin/env python3
"""Pack a directory into a Linux initramfs as a gzip-compressed newc cpio.

Why this exists: `cpio` is not installed in this WSL image, and installing
packages is heavier than this one-file solution.  The newc ("SVR4" / `070701`)
format is simple enough to write directly and is exactly what the kernel's
initramfs unpacker expects.

Usage:  python3 mkinitramfs.py <rootdir> <out.cpio.gz>
"""
import gzip, os, stat, sys, time

NEWC_MAGIC = "070701"

def names_and_modes(root):
    """Yield (abs_path, rel_path, mode) for root and everything under it."""
    for dirpath, dirnames, filenames in os.walk(root):
        # Keep symlinks visible as entries, do not follow them.
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        base = "." if rel_dir == "." else rel_dir
        yield dirpath, base, None
        for f in filenames:
            yield os.path.join(dirpath, f), os.path.join(rel_dir, f), None


def entry_bytes(root, path, rel, ino):
    """Build the 110-byte newc header + name + NUL padding + data."""
    st = os.lstat(path)
    if stat.S_ISDIR(st.st_mode):
        mode = stat.S_IFDIR | 0o755
        data = b""
    elif stat.S_ISLNK(st.st_mode):
        mode = stat.S_IFLNK | 0o777
        data = os.readlink(path).encode()
    elif stat.S_ISREG(st.st_mode):
        mode = stat.S_IFREG | (st.st_mode & 0o7777)
        with open(path, "rb") as f:
            data = f.read()
    else:
        return b""                     # sockets/devices: skip

    # newc requires the name to be NUL-terminated, and `namesize` must
    # INCLUDE that NUL byte.  Omitting it makes the kernel reject the whole
    # archive with "initramfs name without nulterm" / "malformed archive".
    name = rel.encode() + b"\0"
    fields = [
        ino, mode, 0, 0,               # ino, mode, uid, gid
        1, int(st.st_mtime),           # nlink, mtime
        len(data), 0, 0,               # filesize, devmajor, devminor
        0, 0,                          # rdevmajor, rdevminor
        len(name), 0,                  # namesize (INCLUDES the NUL), check
    ]
    hdr = NEWC_MAGIC + "".join(f"{int(v) & 0xFFFFFFFF:08X}" for v in fields)
    assert len(hdr) == 110, len(hdr)

    out = bytearray(hdr.encode())
    out += name
    # Header+name is padded to a 4-byte boundary.
    pad = (-len(out)) % 4
    out += b"\0" * pad
    out += data
    # Data is padded to a 4-byte boundary as well.
    out += b"\0" * ((-len(data)) % 4)
    return bytes(out)


def trailer():
    # A zero-named entry terminates the archive.
    return entry_bytes_raw("TRAILER!!!", 0, 0, b"")


def entry_bytes_raw(name, mode, ino, data):
    # Same NUL-termination rule: namesize must include the trailing NUL.
    nm = name.encode() + b"\0"
    fields = [ino, mode, 0, 0, 1, int(time.time()), len(data), 0, 0, 0, 0,
              len(nm), 0]
    hdr = NEWC_MAGIC + "".join(f"{int(v) & 0xFFFFFFFF:08X}" for v in fields)
    out = bytearray(hdr.encode()) + nm
    out += b"\0" * ((-len(out)) % 4)
    out += data + b"\0" * ((-len(data)) % 4)
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    root, outpath = sys.argv[1], sys.argv[2]

    blob = bytearray()
    ino = 1
    for path, rel, _ in names_and_modes(root):
        if rel == ".":
            rel = "."
        blob += entry_bytes(root, path, rel, ino)
        ino += 1
    blob += trailer()

    with gzip.open(outpath, "wb", compresslevel=9) as f:
        f.write(blob)

    print(f"wrote {outpath}")
    print(f"  entries : {ino - 1}")
    print(f"  raw     : {len(blob)} bytes")
    print(f"  gzipped : {os.path.getsize(outpath)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())