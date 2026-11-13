#!/usr/bin/env python3
"""
check_data_disk.py - verify that build/data.vdi really carries what the
kernel expects to find on the data disk.

The kernel locates the GGUF weights by reading ONE sector (the NEXMODL2
descriptor) from the data disk's secondary IDE channel at LBA 20000, then
pulls the payload from the following sectors.  A mistake in the packing --
wrong LBA, wrong descriptor size, a payload that starts one sector late --
shows up here rather than as a mysterious "model load failed" in the guest.

Usage:
    python3 tools/check_data_disk.py [build/data.vdi] [--seed build/data_vbox.vdi]
Exit codes: 0 ok, 1 mismatch, 2 unreadable.
"""
import os, struct, sys, zlib

SECTOR = 512
MODEL_HDR_LBA = 20000
MKFS_SUPER_LBA = 900
MKFS_DATA_LBA = 917
MKFS_DATA_SECTORS = 15200
MKFS_END = MKFS_DATA_LBA + MKFS_DATA_SECTORS


def read_vdi(path):
    """Return (payload_reader, capacity_bytes, kind).

    Accepts a RAW disk image as well as a VDI container: the raw image is
    what tools/make_data_disk.py produces, and the VDI is what the VM sees
    after VBoxManage convertfromraw.
    """
    import os
    with open(path, 'rb') as f:
        head = f.read(0x200)
        if len(head) < 0x200:
            raise ValueError('%s: too small' % path)
        sig, = struct.unpack_from('<I', head, 0x40)
        if sig != 0xbeda107f:
            size = os.path.getsize(path)

            def raw_reader(lba, n, _p=path):
                with open(_p, 'rb') as g:
                    g.seek(lba * SECTOR)
                    return g.read(n * SECTOR)

            return raw_reader, size, 'raw'

        typ, = struct.unpack_from('<I', head, 0x4C)
        off_blocks, = struct.unpack_from('<I', head, 0x154)
        off_data,   = struct.unpack_from('<I', head, 0x158)
        cap,        = struct.unpack_from('<Q', head, 0x170)
        c_blocks,   = struct.unpack_from('<I', head, 0x180)
        f.seek(off_blocks)
        bat = f.read(c_blocks * 4) if typ == 1 else b''

    def fixed_reader(lba, n):
        with open(path, 'rb') as f:
            f.seek(off_data + lba * SECTOR)
            return f.read(n * SECTOR)

    def dynamic_reader(lba, n):
        out = bytearray(n * SECTOR)
        with open(path, 'rb') as f:
            for i in range(n):
                b = (lba + i) // 2048
                off = (lba + i) % 2048
                if b * 4 + 4 > len(bat):
                    continue
                ent, = struct.unpack_from('<I', bat, b * 4)
                if ent == 0xFFFFFFFF:
                    continue
                f.seek(off_data + ent * (1 << 20) + off * SECTOR)
                out[i * SECTOR:(i + 1) * SECTOR] = f.read(SECTOR)
        return bytes(out)

    reader = fixed_reader if typ == 2 else dynamic_reader
    return reader, cap, ('fixed' if typ == 2 else 'dynamic')


def main():
    path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') \
        else os.path.join('build', 'data_disk.raw')
    seed = None
    if '--seed' in sys.argv:
        seed = sys.argv[sys.argv.index('--seed') + 1]
    if not os.path.exists(path):
        print('MISSING %s -- run "make model-pack"' % path)
        return 2
    try:
        read, cap, kind = read_vdi(path)
    except ValueError as e:
        print('UNREADABLE %s' % e)
        return 2

    print('data disk : %s' % path)
    print('  format  : VDI %s, capacity %.1f MiB (%d sectors)'
          % (kind, cap / 1048576.0, cap // SECTOR))

    rc = 0

    # ---- MKFS superblock ----------------------------------------------------
    sup = read(MKFS_SUPER_LBA, 1)
    if sup[:4] == b'MKFS':
        ver, fcnt, dstart, flba, tsect = struct.unpack_from('<HHIII', sup, 4)
        print('  mkfs    : magic OK, v%d, files=%d, data_start=%d, total=%d'
              % (ver, fcnt, dstart, tsect))
        if ver != 2:
            print('  MKFS version mismatch (want 2)'); rc = 1
    else:
        print('  mkfs    : NO MKFS MAGIC at LBA %d' % MKFS_SUPER_LBA); rc = 1

    # ---- model descriptor ---------------------------------------------------
    desc = read(MODEL_HDR_LBA, 1)
    if desc[:8] != b'NEXMODL2':
        print('  model   : no NEXMODL2 descriptor at LBA %d' % MODEL_HDR_LBA); return 1
    ver, hsz = struct.unpack_from('<II', desc, 8)
    size, slba = struct.unpack_from('<QQ', desc, 16)
    io_base, drv, flags, crc = struct.unpack_from('<HBB I', desc, 32)
    name = desc[40:88].split(b'\x00')[0].decode('utf-8', 'replace')
    print('  model   : NEXMODL2 v%d, header_size=%d, %d bytes, start_lba=%d, '
          'io=0x%X, drive=0x%X, flags=%d, crc32=%08x'
          % (ver, hsz, size, slba, io_base, drv, flags, crc))
    print('            name=%s' % name)
    if hsz != SECTOR:
        print('  header_size must be %d (one sector)' % SECTOR); rc = 1
    if ver != 2:
        print('  version must be 2'); rc = 1
    if io_base != 0x170:
        print('  WARNING: io_base is 0x%X, the kernel expects the secondary '
              'channel 0x170' % io_base); rc = 1
    if slba != MODEL_HDR_LBA + 1:
        print('  start_lba should be %d' % (MODEL_HDR_LBA + 1)); rc = 1
    end_lba = slba + (size + SECTOR - 1) // SECTOR - 1
    if end_lba >= cap // SECTOR:
        print('  payload does not fit in the disk'); rc = 1

    # ---- payload head -------------------------------------------------------
    head = read(slba, 8)
    if head[:4] != b'GGUF':
        print('  payload : head is %r, expected b"GGUF"' % head[:4]); rc = 1
    else:
        c = zlib.crc32(head) & 0xFFFFFFFF
        print('  payload : GGUF magic OK at LBA %d, head crc32=%08x %s'
              % (slba, c, 'MATCH' if c == crc else 'MISMATCH (want %08x)' % crc))
        if c != crc:
            rc = 1

    # ---- seed round-trip ----------------------------------------------------
    if seed and os.path.exists(seed):
        try:
            sread, scap, skind = read_vdi(seed)
            a = sread(MKFS_SUPER_LBA, 1)
            b = read(MKFS_SUPER_LBA, 1)
            same = a[:SECTOR] == b[:SECTOR]
            print('  seed    : MKFS superblock from %s %s'
                  % (seed, 'preserved' if same else 'DIFFERS'))
        except ValueError as e:
            print('  seed    : cannot read %s (%s)' % (seed, e))

    print('RESULT: %s' % ('OK -- the kernel will find this model' if rc == 0
                           else 'MISMATCH -- do not flash this disk'))
    return rc


if __name__ == '__main__':
    sys.exit(main())
