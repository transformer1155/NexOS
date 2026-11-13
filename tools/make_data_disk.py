#!/usr/bin/env python3
"""
make_data_disk.py - build the NexOS DATA disk as a RAW disk image.

WHY RAW AND NOT A VDI CONTAINER
--------------------------------
The first version of this tool wrote a VirtualBox VDI container itself, using
the classic qemu VDI 1.1 header layout (fields packed from offset 0x40).  That
layout is NOT what VirtualBox writes or reads: VirtualBox uses a 400-byte
header whose comment field occupies 0x54..0x153, with offBlocks at 0x154,
offData at 0x158 and the disk size at 0x170.  A container written the old way
is not opened by VirtualBox -- and, worse, tools/vbox_check.py silently
compared an EMPTY payload and reported "IN SYNC" for a disk it had never
actually inspected.

So this tool now emits a plain RAW disk image and lets VBoxManage
(convertfromraw) build the container, exactly like tools/vbox_sync.ps1 does
for the system disk.  The authoritative converter is the one on the host.

LAYOUT
------
    LBA 900            MKFS superblock            (byte-identical to before)
    LBA 901..916       MKFS file table
    LBA 917..16116     MKFS data area
    LBA 16117..19999   reserved
    LBA 20000          NEXMODL2 descriptor        (one sector)
    LBA 20001..        GGUF payload

The write is STREAMED: a 512 MiB disk is produced without holding more than
1 MiB in memory, so the tool runs on the Windows host as well as in the VM.

Usage:
    python3 tools/make_data_disk.py --out build/data_disk.raw
    python3 tools/make_data_disk.py --size-mb 512 \
           --model build/qwen2-0_5b-instruct-q4_k_m.gguf \
           --seed build/data_vbox.vdi --out build/data_disk.raw
"""
import argparse, os, struct, sys, zlib

SECTOR_SIZE = 512

# ---- MKFS layout (must match kernel.cpp) ----
# MKFS_SUPER_LBA is 900, NOT 512: LBA 512 sits inside kernel32.bin on the
# boot disk, so formatting there corrupts the kernel.
SUPER_LBA    = 900
TABLE_LBA    = 901
TABLE_SECT   = 16
DATA_LBA     = 917
DATA_SECTORS = 15200
MKFS_VERSION = 2

# ---- model descriptor v2 ----
MODEL_MAGIC     = b'NEXMODL2'
MODEL_HDR_LBA   = 20000
MODEL_NAME_MAX  = 48
MODEL_DESC_SIZE = 512        # must match sizeof(ModelDescV2) in kernel64.cpp

MKFS_END_SECTOR  = DATA_LBA + DATA_SECTORS          # 16117
MKFS_REGION_BYTES = MKFS_END_SECTOR * SECTOR_SIZE


def build_mkfs_super():
    """Return the MKFS superblock bytes (matches struct Superblock)."""
    return struct.pack('<4sHHIII',
                       b'MKFS',            # magic
                       MKFS_VERSION,      # version (must match MKFS_V2)
                       0,                 # file_count
                       DATA_LBA,          # data_start
                       DATA_LBA,          # free_lba
                       DATA_SECTORS,      # total_sectors
                       )


def build_desc(model_path, payload_size, start_lba, io_base, drive, name):
    """Pack the NEXMODL2 descriptor (must match struct ModelDescV2)."""
    with open(model_path, 'rb') as f:
        head = f.read(4096)
    crc = zlib.crc32(head) & 0xFFFFFFFF
    nm = (name or os.path.basename(model_path))[:MODEL_NAME_MAX - 1]
    nb = nm.encode('utf-8', 'replace')[:MODEL_NAME_MAX - 1]
    desc = struct.pack('<8sIIQQHBBI48s424s',
                       MODEL_MAGIC, 2, MODEL_DESC_SIZE,
                       payload_size, start_lba,
                       io_base, drive, 1, crc,
                       nb, b'\0' * 424)
    assert len(desc) == MODEL_DESC_SIZE, len(desc)
    return desc, crc


def write_zeros(f, n):
    CHUNK = 1 << 20
    z = b'\0' * CHUNK
    while n > 0:
        k = CHUNK if n > CHUNK else n
        f.write(z[:k])
        n -= k


def vdi_payload_prefix(path, nbytes):
    """Return the first nbytes of a VDI container's payload (or of a raw file).

    Handles both fixed and dynamic VDIs, using VirtualBox's OWN header layout
    (comment at 0x54, offBlocks at 0x154, offData at 0x158, disk size at
    0x170).  Only the requested prefix is read, so a 512 MiB container never
    has to fit in memory.
    """
    with open(path, 'rb') as f:
        head = f.read(0x200)
        if len(head) < 0x200:
            raise ValueError('%s: too small' % path)
        sig, = struct.unpack_from('<I', head, 0x40)
        if sig != 0xbeda107f:
            f.seek(0)
            data = f.read(nbytes)
            return data + b'\0' * max(0, nbytes - len(data))
        image_type, = struct.unpack_from('<I', head, 0x4C)
        off_blocks, = struct.unpack_from('<I', head, 0x154)
        off_data,   = struct.unpack_from('<I', head, 0x158)
        cb_disk,    = struct.unpack_from('<Q', head, 0x170)
        if image_type == 2:                      # fixed
            f.seek(off_data)
            return f.read(nbytes)
        if image_type == 1:                      # dynamic
            c_blocks, = struct.unpack_from('<I', head, 0x180)
            f.seek(off_blocks)
            bat = f.read(c_blocks * 4)
            out = bytearray(nbytes)
            for b in range(c_blocks):
                ent, = struct.unpack_from('<I', bat, b * 4)
                if ent == 0xFFFFFFFF:
                    continue
                base = b * (1 << 20)
                if base >= nbytes:
                    break
                f.seek(off_data + ent * (1 << 20))
                chunk = f.read(min(1 << 20, nbytes - base))
                out[base:base + len(chunk)] = chunk
            return bytes(out)
        raise ValueError('%s: unsupported VDI type %d' % (path, image_type))


def build(out, total_sectors, model_path=None, model_lba=MODEL_HDR_LBA,
          seed=None, io_base=0x170, drive=0xE0, name=None):
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    tmp = out + '.new'
    with open(tmp, 'wb') as f:
        # ---- region 1: the MKFS filesystem (byte-identical to before) ----
        region = bytearray(MKFS_REGION_BYTES)
        if seed and os.path.exists(seed):
            got = vdi_payload_prefix(seed, MKFS_REGION_BYTES)
            region[0:len(got)] = got
            print('  seeded MKFS region from %s (%d bytes)' % (seed, len(got)))
        else:
            sb = build_mkfs_super()
            region[SUPER_LBA * SECTOR_SIZE:SUPER_LBA * SECTOR_SIZE + len(sb)] = sb
            print('  fresh MKFS superblock written')
        f.write(bytes(region))

        if model_path:
            payload_bytes = os.path.getsize(model_path)
            payload_sectors = (payload_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
            start_lba = model_lba + 1
            need = start_lba + payload_sectors
            if need > total_sectors:
                raise SystemExit('model does not fit: needs %d sectors, disk has %d'
                                 % (need, total_sectors))
            pad = (model_lba - MKFS_END_SECTOR) * SECTOR_SIZE
            if pad > 0:
                write_zeros(f, pad)
            desc, crc = build_desc(model_path, payload_bytes, start_lba,
                                   io_base, drive, name)
            f.write(desc)
            print('  descriptor: %s, %d bytes payload, lba %d..%d, crc32=%08x'
                  % (MODEL_MAGIC.decode(), payload_bytes, start_lba,
                     start_lba + payload_sectors - 1, crc))
            with open(model_path, 'rb') as m:
                while True:
                    chunk = m.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
            tail = (payload_sectors * SECTOR_SIZE) - payload_bytes
            if tail > 0:
                write_zeros(f, tail)
            written = need
        else:
            written = MKFS_END_SECTOR

        rest = total_sectors - written
        if rest > 0:
            write_zeros(f, rest * SECTOR_SIZE)
        f.truncate(total_sectors * SECTOR_SIZE)
    os.replace(tmp, out)
    return total_sectors * SECTOR_SIZE


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--size-mb', type=int, default=0,
                    help='disk size in MiB (default: 8 without a model, 512 with one)')
    ap.add_argument('--model', default=None, help='GGUF file to pack onto the disk')
    ap.add_argument('--model-lba', type=int, default=MODEL_HDR_LBA,
                    help='LBA of the NEXMODL2 descriptor (default %d)' % MODEL_HDR_LBA)
    ap.add_argument('--seed', default=None,
                    help='existing disk (VDI or raw) whose MKFS region is preserved')
    ap.add_argument('--out', default=os.path.join('build', 'data_disk.raw'),
                    help='output raw image (default build/data_disk.raw)')
    ap.add_argument('--name', default=None, help='model name recorded in the descriptor')
    args = ap.parse_args()

    size_mb = args.size_mb or (512 if args.model else 8)
    total_sectors = size_mb * 1024 * 1024 // SECTOR_SIZE
    if SUPER_LBA + 1 + TABLE_SECT + DATA_SECTORS > total_sectors:
        raise SystemExit('disk too small for the MKFS data area')
    print('make_data_disk: %d MiB, %d sectors -> %s' % (size_mb, total_sectors, args.out))
    n = build(args.out, total_sectors, model_path=args.model,
              model_lba=args.model_lba, seed=args.seed, name=args.name)
    print('Wrote %s (%d bytes, raw)' % (args.out, n))


if __name__ == '__main__':
    main()
