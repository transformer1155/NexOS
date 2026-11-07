#!/usr/bin/env python3
"""
vbox_sync.py - Keep the VirtualBox disk containers in step with the raw
images this repo builds.

Why this exists
---------------
build/os_v2_vbox.vdi and build/data_vbox.vdi are REAL VirtualBox VDI
containers (header "<<< Oracle VirtualBox Disk Image >>>"), not copies of
the raw .img files -- so you cannot sync them with cp.  They were previously
produced by hand, which is exactly how they drifted: a freshly built
build/os_v2.img (13:48) sat next to a 11:31 vdi, the VM booted the stale
one, and every fix merged after 11:31 appeared to be missing in VirtualBox.

This script regenerates the VDI payload from the raw image, preserving the
VDI container format so the VM keeps booting it.  It works in place on a
copy first and then swaps, so a failed conversion cannot leave the VM with
a truncated disk.

Usage:
    python3 tools/vbox_sync.py [--raw build/os_v2.img] [--vdi build/os_v2_vbox.vdi]
"""
import argparse, os, struct, sys, shutil, uuid

VDI_SIGNATURE = 0xbeda107f
VDI_VERSION   = 0x00010001
VDI_TYPE_DYNAMIC = 1
VDI_TYPE_STATIC  = 2
HEADER_SIZE = 512
BLOCK_SIZE  = 1 << 20
SECTOR_SIZE = 512


def read_vdi_payload(path):
    """Return (payload_bytes, header) from a VDI file (static or dynamic)."""
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) < HEADER_SIZE:
        raise SystemExit('%s: too small to be a VDI' % path)
    sig = struct.unpack_from('<I', raw, 0)[0]
    if sig != VDI_SIGNATURE:
        raise SystemExit('%s: not a VDI (first4=%08x)' % (path, sig))
    hdr = raw[:HEADER_SIZE]
    image_type, = struct.unpack_from('<I', hdr, 0x38)
    offset_data, = struct.unpack_from('<Q', hdr, 0x48)
    offset_blocks, = struct.unpack_from('<I', hdr, 0x154)
    blocks, = struct.unpack_from('<I', hdr, 0x158)
    disk_size, = struct.unpack_from('<Q', hdr, 0x170)
    if image_type == VDI_TYPE_STATIC:
        payload = raw[offset_data:offset_data + disk_size]
    elif image_type == VDI_TYPE_DYNAMIC:
        payload = bytearray(disk_size)
        for b in range(blocks):
            ent, = struct.unpack_from('<I', raw, offset_blocks + b * 4)
            if ent == 0xFFFFFFFF:
                continue
            off = offset_data + ent * SECTOR_SIZE
            chunk = raw[off:off + BLOCK_SIZE]
            base = b * BLOCK_SIZE
            payload[base:base + len(chunk)] = chunk
        payload = bytes(payload)
    else:
        raise SystemExit('%s: unsupported VDI type %d' % (path, image_type))
    return payload, hdr


def build_vdi_static(payload, template_hdr):
    """Rebuild a STATIC VDI around payload using template_hdr's geometry."""
    disk_size, = struct.unpack_from('<Q', template_hdr, 0x170)
    if len(payload) > disk_size:
        raise SystemExit('payload %d bytes exceeds VDI capacity %d -- '
                         'the VM disk must be recreated larger'
                         % (len(payload), disk_size))
    padded = payload + bytes(disk_size - len(payload))
    hdr = bytearray(template_hdr)
    struct.pack_into('<I', hdr, 0x38, VDI_TYPE_STATIC)
    struct.pack_into('<I', hdr, 0x154, 0xFFFFFFFF)   # no block table
    struct.pack_into('<I', hdr, 0x158, 0)
    struct.pack_into('<Q', hdr, 0x48, HEADER_SIZE)    # data starts right after header
    struct.pack_into('<Q', hdr, 0x170, disk_size)
    # VDI header checksum: sum of every 32-bit word from 0x40 onward.
    for off in (0x40, 0x50):
        struct.pack_into('<I', hdr, off, 0)
    csum = 0
    for i in range(0x40, HEADER_SIZE, 4):
        csum = (csum + struct.unpack_from('<I', hdr, i)[0]) & 0xFFFFFFFF
    struct.pack_into('<I', hdr, 0x40, csum)
    return bytes(hdr) + padded


def sync(raw_path, vdi_path):
    if not os.path.exists(raw_path):
        raise SystemExit('%s: missing (build it first)' % raw_path)
    if not os.path.exists(vdi_path):
        raise SystemExit('%s: missing (create the VM disk once, then re-run)' % vdi_path)
    raw = open(raw_path, 'rb').read()
    _, tmpl = read_vdi_payload(vdi_path)
    out = build_vdi_static(raw, tmpl)
    tmp = vdi_path + '.new'
    with open(tmp, 'wb') as f:
        f.write(out)
    old_payload, _ = read_vdi_payload(vdi_path)
    os.replace(tmp, vdi_path)
    return len(raw), len(out), old_payload[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--raw', default='build/os_v2.img')
    ap.add_argument('--vdi', default='build/os_v2_vbox.vdi')
    a = ap.parse_args()
    n_raw, n_vdi, old_head = sync(a.raw, a.vdi)
    print('  %s -> %s' % (a.raw, a.vdi))
    print('  raw payload %d bytes, VDI %d bytes' % (n_raw, n_vdi))
    print('  previous VDI payload started with %s' % old_head.hex())
    print('  new      VDI payload starts with %s'
          % open(a.raw, 'rb').read(16).hex())
    return 0


if __name__ == '__main__':
    sys.exit(main())
