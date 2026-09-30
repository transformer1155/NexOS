#!/usr/bin/env python3
"""mk_initramfs.py - build a Linux initramfs (newc cpio + gzip) from a dir.

Usage: python3 mk_initramfs.py <source_dir> <output.img>

Produces a gzip-compressed initramfs archive in the standard Linux newc
format (deterministic: mtime=0).  Each newc entry header is exactly 110
bytes: 6-byte magic '070701' + 13 x 8 hex fields.
"""
import os
import sys
import gzip
import stat

def pad_to(data, n, align=4):
    while len(data) % align != 0:
        data += b'\x00'
    return data

def newc_entry(name_b, mode, uid, gid, nlink, mtime, size,
               devmajor, devminor, rdevmajor, rdevminor):
    namesize = len(name_b) + 1  # includes NUL terminator
    fields = [
        format(0, '08x'),            # ino
        format(mode, '08x'),
        format(uid, '08x'),
        format(gid, '08x'),
        format(nlink, '08x'),
        format(mtime, '08x'),
        format(size, '08x'),
        format(devmajor, '08x'),
        format(devminor, '08x'),
        format(rdevmajor, '08x'),
        format(rdevminor, '08x'),
        format(namesize, '08x'),
        format(0, '08x'),            # check
    ]
    hdr = b'070701' + ''.join(fields).encode()
    assert len(hdr) == 110, len(hdr)
    # Header is exactly 110 bytes, immediately followed by the name (NUL
    # terminated).  The byte stream is aligned to 4 only BEFORE the data.
    return hdr + name_b + b'\x00'

def pack(src, out):
    out = os.path.abspath(out)
    entries = []
    for root, dirs, files in os.walk(src):
        dirs.sort(); files.sort()
        dirname = '' if root == src else os.path.relpath(root, src)
        for d in sorted(dirs):
            p = os.path.join(root, d)
            entries.append((os.path.join(dirname, d), 0o040755))
        for f in files:
            p = os.path.join(root, f)
            st = os.lstat(p)
            if stat.S_ISSOCK(st.st_mode):
                continue
            entries.append((os.path.join(dirname, f), None))

    buf = bytearray()
    for name, dmode in entries:
        full = os.path.join(src, name)
        st = os.lstat(full)
        if stat.S_ISLNK(st.st_mode):
            mode = 0o0120777
            data = os.readlink(full).encode()
        elif stat.S_ISDIR(st.st_mode):
            mode = 0o040755
            data = b''
        elif stat.S_ISREG(st.st_mode):
            mode = st.st_mode
            with open(full, 'rb') as fh:
                data = fh.read()
        else:
            mode = 0o0100644
            data = b''
        buf += newc_entry(name.encode(), mode, 0, 0, 1, 0, len(data),
                          0, 0, 0, 0)
        buf = pad_to(buf, 4)
        buf += data
        buf = pad_to(buf, 4)

    # End of archive: 512 zero bytes then the TRAILER entry.
    buf += b'\x00' * 512
    buf += newc_entry(b'TRAILER!!!', 0, 0, 0, 1, 0, 0, 0, 0, 0, 0)
    buf = pad_to(buf, 4)

    with gzip.open(out, 'wb', compresslevel=1) as gz:
        gz.write(bytes(buf))
    return len(buf)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('usage: mk_initramfs.py <src_dir> <out.img>')
        sys.exit(1)
    n = pack(sys.argv[1], sys.argv[2])
    print(f'packed {n} bytes (uncompressed) -> {os.path.basename(sys.argv[2])} '
          f'{os.path.getsize(sys.argv[2])} bytes')