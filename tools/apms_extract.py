#!/usr/bin/env python3
"""Extract APMS artefacts from the NexOS MKFS data disk.

The APMS kernel subsystem writes two files into the MKFS data volume:

  apms_report.md   Markdown bug report (runtime info + user description)
  apms_shot.ppm    P6 PPM screenshot captured from the VBE framebuffer

This tool reads the data disk image (raw .img or VirtualBox .vdi), parses the
MKFS superblock + file table, extracts both files and converts the PPM into a
PNG so it can be attached to a bug report.

Usage:
    python tools/apms_extract.py [--disk build/data_vbox.vdi] [--out out]

--disk accepts either a raw image (default) or a VDI; VDI files are detected
by their signature.  If the disk has no APMS files the tool exits 0 with a
message (nothing to extract is not an error).
"""

import argparse
import os
import struct
import sys
import zlib

SECTOR = 512

MKFS_SUPER_LBA = 900
MKFS_TABLE_LBA = 901
MKFS_TABLE_SECT = 16
FS_ENTRY_SIZE = 32
FS_NAME_LEN = 20
FS_ENTRY_PER_SEC = 16
FS_TYPE_FILE = 0

VDI_SIGNATURE = 0xbeda107f
VDI_TYPE_DYNAMIC = 1
VDI_TYPE_STATIC = 2
VDI_HEADER_SIZE = 512
VDI_BLOCK_SIZE = 1 << 20
# The signature lives at 0x40 (VDI 1.1 header: the first bytes are the
# "<<< Oracle VirtualBox Disk Image >>>" text banner).
VDI_SIG_OFF = 0x40


def read_vdi_payload(path):
    """Return the raw disk payload of a VDI (static or dynamic).

    Header layout matches tools/vbox_check.py (the container VirtualBox
    actually produces for this project): image type at 0x4C, block map at
    0x154, block data at 0x158, disk size at 0x170, block count at 0x180.
    Block-map entries are BLOCK numbers (1 MiB), not sector numbers.
    """
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) < 0x200:
        raise SystemExit('%s: too small to be a VDI' % path)
    sig, = struct.unpack_from('<I', raw, VDI_SIG_OFF)
    if sig != VDI_SIGNATURE:
        raise SystemExit('%s: not a VDI (sig@0x40=%08x)' % (path, sig))
    image_type, = struct.unpack_from('<I', raw, 0x4C)
    off_blocks, = struct.unpack_from('<I', raw, 0x154)
    off_data, = struct.unpack_from('<I', raw, 0x158)
    disk_size, = struct.unpack_from('<Q', raw, 0x170)
    if image_type == VDI_TYPE_STATIC:
        return raw[off_data:off_data + disk_size]
    if image_type == VDI_TYPE_DYNAMIC:
        blocks, = struct.unpack_from('<I', raw, 0x180)
        payload = bytearray(disk_size)
        for b in range(blocks):
            ent, = struct.unpack_from('<I', raw, off_blocks + b * 4)
            if ent == 0xFFFFFFFF:
                continue
            src = off_data + ent * VDI_BLOCK_SIZE
            chunk = raw[src:src + VDI_BLOCK_SIZE]
            base = b * VDI_BLOCK_SIZE
            payload[base:base + len(chunk)] = chunk
        return bytes(payload)
    raise SystemExit('%s: unsupported VDI type %d' % (path, image_type))


def load_disk(path):
    with open(path, 'rb') as f:
        head = f.read(0x200)
    if len(head) >= 0x44:
        sig, = struct.unpack_from('<I', head, VDI_SIG_OFF)
        if sig == VDI_SIGNATURE:
            return read_vdi_payload(path)
    with open(path, 'rb') as f:
        return f.read()


def parse_mkfs(disk):
    """Return {name: bytes} for every regular file in the MKFS volume."""
    sb_off = MKFS_SUPER_LBA * SECTOR
    if len(disk) < sb_off + SECTOR:
        return {}
    magic = disk[sb_off:sb_off + 4]
    if magic != b'MKFS':
        return {}
    files = {}
    for s in range(MKFS_TABLE_SECT):
        base = (MKFS_TABLE_LBA + s) * SECTOR
        for e in range(FS_ENTRY_PER_SEC):
            off = base + e * FS_ENTRY_SIZE
            if off + FS_ENTRY_SIZE > len(disk):
                continue
            name_b = disk[off:off + FS_NAME_LEN]
            if name_b[0] == 0:
                continue
            nul = name_b.find(b'\x00')
            name = name_b[:nul if nul >= 0 else FS_NAME_LEN].decode('utf-8', 'replace')
            size, start_lba = struct.unpack_from('<II', disk, off + FS_NAME_LEN)
            ftype = disk[off + FS_NAME_LEN + 8]
            if ftype != FS_TYPE_FILE:
                continue
            data_off = start_lba * SECTOR
            if data_off + size > len(disk):
                size = max(0, len(disk) - data_off)
            files[name] = disk[data_off:data_off + size]
    return files


def ppm_to_png(ppm_bytes):
    """Convert a binary P6 PPM into PNG bytes."""
    if not ppm_bytes.startswith(b'P6'):
        raise SystemExit('not a P6 PPM')
    # header: P6 <w> <h> <maxval>\n  (whitespace separated, comments allowed)
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(ppm_bytes) and ppm_bytes[idx:idx + 1].isspace():
            idx += 1
        if ppm_bytes[idx:idx + 1] == b'#':
            while idx < len(ppm_bytes) and ppm_bytes[idx] != 0x0A:
                idx += 1
            continue
        start = idx
        while idx < len(ppm_bytes) and not ppm_bytes[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(ppm_bytes[start:idx]))
    idx += 1  # single whitespace after maxval
    w, h, maxval = vals
    if maxval != 255:
        raise SystemExit('unsupported PPM maxval %d' % maxval)
    rgb = ppm_bytes[idx:idx + w * h * 3]
    if len(rgb) < w * h * 3:
        rgb = rgb + b'\x00' * (w * h * 3 - len(rgb))

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    return png


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disk', default='build/data_vbox.vdi',
                    help='data disk image (raw .img or .vdi)')
    ap.add_argument('--out', default='build/apms',
                    help='output directory for extracted artefacts')
    args = ap.parse_args()

    if not os.path.exists(args.disk):
        raise SystemExit('%s: missing' % args.disk)
    disk = load_disk(args.disk)
    files = parse_mkfs(disk)
    if not files:
        print('APMS: no MKFS volume found in %s (nothing to extract)' % args.disk)
        return 0

    os.makedirs(args.out, exist_ok=True)
    found = 0

    if 'apms_report.md' in files:
        dst = os.path.join(args.out, 'apms_report.md')
        with open(dst, 'wb') as f:
            f.write(files['apms_report.md'])
        print('APMS: wrote %s (%d bytes)' % (dst, len(files['apms_report.md'])))
        found += 1

    if 'apms_shot.ppm' in files:
        ppm = files['apms_shot.ppm']
        raw_dst = os.path.join(args.out, 'apms_shot.ppm')
        with open(raw_dst, 'wb') as f:
            f.write(ppm)
        try:
            png = ppm_to_png(ppm)
            png_dst = os.path.join(args.out, 'apms_shot.png')
            with open(png_dst, 'wb') as f:
                f.write(png)
            print('APMS: wrote %s (%d bytes) and %s (%d bytes)'
                  % (raw_dst, len(ppm), png_dst, len(png)))
        except SystemExit as e:
            print('APMS: wrote %s but PNG conversion failed: %s' % (raw_dst, e))
        found += 1

    if not found:
        print('APMS: MKFS volume mounted but no apms_report.md / apms_shot.ppm found')
        print('APMS: files present: %s' % ', '.join(sorted(files.keys())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
