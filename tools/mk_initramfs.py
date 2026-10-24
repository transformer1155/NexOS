#!/usr/bin/env python3
"""mk_initramfs.py - build a Linux initramfs (newc cpio + gzip) from a dir.

Usage: python3 mk_initramfs.py <source_dir> <output.img>

Produces a gzip-compressed initramfs archive in the standard Linux
``newc`` format (deterministic: mtime=0).  Each newc entry header is
exactly 110 bytes: 6-byte magic ``070701`` + 13 x 8-hex-digit fields.

Layout of one newc entry (from ``init/initramfs.c`` / ``Documentation``):
    magic     : 6 bytes  "070701"
    header    : 13 * 8 = 104 bytes of zero-padded 8-digit hex
    name      : NUL-terminated, then padded with NULs to a 4-byte boundary
    data      : the file contents, then padded with NULs to a 4-byte boundary
The whole stream ends with a 512-byte zero block followed by a
``TRAILER!!!`` entry.
"""
import os
import sys
import gzip
import stat

ALIGN = 4


def _pad(data: bytes) -> bytes:
    """Pad *data* with NUL bytes so its length is a multiple of ALIGN."""
    rem = len(data) % ALIGN
    if rem:
        data += b"\x00" * (ALIGN - rem)
    return data


def newc_header(name: bytes, mode: int, size: int,
                rdevmajor: int = 0, rdevminor: int = 0) -> bytes:
    """Build a 110-byte newc header followed by the NUL-terminated name."""
    namesize = len(name) + 1  # counts the trailing NUL
    fields = [
        format(0, "08x"),            # ino
        format(mode & 0o7777, "08x"),
        format(0, "08x"),            # uid
        format(0, "08x"),            # gid
        format(1, "08x"),            # nlink
        format(0, "08x"),            # mtime (0 => reproducible)
        format(size, "08x"),
        format(0, "08x"),            # devmajor
        format(0, "08x"),            # devminor
        format(rdevmajor, "08x"),    # rdevmajor (for device nodes)
        format(rdevminor, "08x"),    # rdevminor
        format(namesize, "08x"),
        format(0, "08x"),            # check
    ]
    hdr = b"070701" + "".join(fields).encode()
    assert len(hdr) == 110, len(hdr)
    # Header is exactly 110 bytes, immediately followed by the name (NUL
    # terminated).  The name region is padded *before* the data begins.
    return hdr + name + b"\x00"


def pack(src: str, out: str) -> int:
    src = os.path.abspath(src)
    out = os.path.abspath(out)

    # Collect (relative_path, mode, data) tuples.  Directories and symlinks
    # are emitted before the files that live inside them so the kernel's
    # extractor can create parents first.
    entries = []  # (name:str, mode:int, data:bytes, rmaj, rmin)
    for root, dirs, files in os.walk(src):
        dirs.sort()
        files.sort()
        rel_dir = "" if root == src else os.path.relpath(root, src)
        for d in sorted(dirs):
            p = os.path.join(root, d)
            name = os.path.join(rel_dir, d) if rel_dir else d
            entries.append((name, 0o040755, b"", 0, 0))
        for f in sorted(files):
            p = os.path.join(root, f)
            name = os.path.join(rel_dir, f) if rel_dir else f
            st = os.lstat(p)
            if stat.S_ISSOCK(st.st_mode):
                continue  # sockets cannot live in an initramfs
            if stat.S_ISLNK(st.st_mode):
                data = os.readlink(p).encode()
                entries.append((name, 0o0120777, data, 0, 0))
            elif stat.S_ISDIR(st.st_mode):
                entries.append((name, 0o040755, b"", 0, 0))
            elif stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
                rmaj = os.major(st.st_rdev)
                rmin = os.minor(st.st_rdev)
                entries.append((name, st.st_mode & 0o7777, b"", rmaj, rmin))
            else:  # regular file (or unknown -> treat as file)
                with open(p, "rb") as fh:
                    data = fh.read()
                entries.append((name, st.st_mode & 0o7777, data, 0, 0))

    if not any(e[0] == "init" for e in entries):
        raise SystemExit("ERROR: /init not found in source tree; "
                         "busybox must be copied to <root>/init")

    buf = bytearray()
    for name, mode, data, rmaj, rmin in entries:
        name_b = name.encode()
        buf += newc_header(name_b, mode, len(data), rmaj, rmin)
        buf = _pad(buf)          # pad the name region to 4 bytes
        buf += data
        buf = _pad(buf)          # pad the data region to 4 bytes

    # End of archive: a 512-byte zero block, then the TRAILER entry
    # (the kernel looks for a zero-magic header to stop extracting).
    buf += b"\x00" * 512
    buf += newc_header(b"TRAILER!!!", 0, 0)
    buf = _pad(buf)

    with gzip.open(out, "wb", compresslevel=9) as gz:
        gz.write(bytes(buf))
    return len(buf)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: mk_initramfs.py <src_dir> <out.img>")
        sys.exit(1)
    n = pack(sys.argv[1], sys.argv[2])
    print(f"packed {n} bytes (uncompressed) -> "
          f"{os.path.basename(sys.argv[2])} ({os.path.getsize(sys.argv[2])} bytes)")
