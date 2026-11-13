#!/usr/bin/env python3
"""
vbox_check.py - Report whether the VirtualBox disk containers still match
the raw images this repo builds.  Read-only; safe on any platform; needs no
VBoxManage.

Why a *payload* comparison and not a file hash
----------------------------------------------
build/os_v2_vbox.vdi and build/data_vbox.vdi are real VirtualBox VDI
containers (dynamic, 1 MiB blocks, signature 0xBEDA107F at offset 0x40), so
their bytes are nothing like the raw images.  Comparing the two files
directly would always report "different" and teach the reader to ignore the
warning.  So this script extracts the container payload and compares it
against the raw image byte for byte, which is the only comparison that
answers the question actually being asked: "would the VM boot the build I
just made?"

Memory
------
The data disk is 512 MiB now (it carries the GGUF weights), so nothing is
read whole: both sides are served by random-access readers and compared in
1 MiB chunks.

Exit codes (the contract the Makefile and CI rely on):
    0  containers match the current build
    1  containers are STALE -- run 'make vbox-sync' on the Windows host
    2  containers are missing or unreadable (cannot answer the question)
    3  the container capacity is too small for the raw image
"""
import os, struct, sys

VDI_SIGNATURE = 0xbeda107f
VDI_TYPE_DYNAMIC = 1
VDI_TYPE_STATIC  = 2
BLOCK_SIZE = 1 << 20
SECTOR_SIZE = 512
HDR_SIG_OFF = 0x40

# Sectors the GUEST writes on purpose, from kernel64.cpp.  A live run leaves
# these dirty, so comparing them to the pristine image would report "stale"
# forever and train the reader to ignore the warning -- the exact failure this
# checker exists to prevent.  They must be skipped, not compared.
#
#   SVM_DIAG_LBA  = 1500  fault_common()/diag_step() record sector
#                           (an aborted 64-bit boot leaves "ENDX" + "rend" here,
#                            which is how a byte-diff at offset 0xBB800 looked
#                            like a stale container on 2026-11-06)
#   SVM_LOG_LBA   = 2047  svm log, one sector below kernel64.bin
SYSTEM_GUEST_WRITABLE = {1500, 2047}

# The DATA disk carries a LIVE MKFS filesystem (LBA 900..16116): the guest
# writes user files, desktop shortcuts and session state there on every run.
# Those sectors are dirty by design and must be skipped, or the checker would
# report the data disk as stale after the very first boot.
DATA_MKFS_SKIP = set(range(900, 16117))

# (source, container, skip_sectors).  Both sources are RAW disk images; both
# containers are VDI files whose PAYLOAD is what has to match.
#
# The data disk used to be compared against build/data.vdi -- a hand-rolled
# VDI in the classic qemu header layout -- which VirtualBox cannot open and
# which this parser reduced to an EMPTY payload.  The comparison was silently
# vacuous: it reported "IN SYNC" for a disk nobody had ever inspected.  The
# source is now the raw image the converter turns into the container.
TARGETS = [
    ('build/os_v2.img',      'build/os_v2_vbox.vdi', SYSTEM_GUEST_WRITABLE),
    ('build/data_disk.raw', 'build/data_vbox.vdi',  DATA_MKFS_SKIP),
]

CHUNK_SECTORS = 2048          # 1 MiB


def _reader_for(path, off_data, bat):
    """Return a function (lba, nsectors) -> bytes reading the payload."""
    def read(lba, n):
        out = bytearray(n * SECTOR_SIZE)
        with open(path, 'rb') as f:
            done = 0
            while done < n:
                cur = lba + done
                if bat is None:                      # raw or fixed container
                    f.seek(off_data + cur * SECTOR_SIZE)
                    chunk = f.read((n - done) * SECTOR_SIZE)
                    out[done * SECTOR_SIZE:done * SECTOR_SIZE + len(chunk)] = chunk
                    if not chunk:
                        break
                    done += len(chunk) // SECTOR_SIZE
                    continue
                b, off = divmod(cur, BLOCK_SIZE // SECTOR_SIZE)
                if b * 4 + 4 > len(bat):
                    done += 1
                    continue
                ent, = struct.unpack_from('<I', bat, b * 4)
                if ent == 0xFFFFFFFF:
                    done += 1
                    continue
                f.seek(off_data + ent * BLOCK_SIZE + off * SECTOR_SIZE)
                chunk = f.read((n - done) * SECTOR_SIZE)
                out[done * SECTOR_SIZE:done * SECTOR_SIZE + len(chunk)] = chunk
                if not chunk:
                    break
                done += len(chunk) // SECTOR_SIZE
        return bytes(out)
    return read


def open_disk(path):
    """Return (reader, capacity_bytes) for a raw image or a VDI container."""
    with open(path, 'rb') as f:
        head = f.read(0x200)
        if len(head) < 0x200:
            raise ValueError('%s: too small' % path)
        sig, = struct.unpack_from('<I', head, HDR_SIG_OFF)
        if sig != VDI_SIGNATURE:
            size = os.path.getsize(path)
            return _reader_for(path, 0, None), size
        image_type, = struct.unpack_from('<I', head, 0x4C)
        off_blocks, = struct.unpack_from('<I', head, 0x154)
        off_data,   = struct.unpack_from('<I', head, 0x158)
        cb_disk,    = struct.unpack_from('<Q', head, 0x170)
        if image_type == VDI_TYPE_STATIC:
            return _reader_for(path, off_data, None), cb_disk
        if image_type == VDI_TYPE_DYNAMIC:
            c_blocks, = struct.unpack_from('<I', head, 0x180)
            f.seek(off_blocks)
            bat = f.read(c_blocks * 4)
            return _reader_for(path, off_data, bat), cb_disk
        raise ValueError('%s: unsupported VDI type %d' % (path, image_type))


def compare(src_path, ctr_path, skip_lbas):
    """Compare the two payloads.  Returns (status, detail)."""
    src_read, src_cap = open_disk(src_path)
    ctr_read, ctr_cap = open_disk(ctr_path)
    if src_cap > ctr_cap:
        return 'TOO SMALL', 'image %d > capacity %d bytes' % (src_cap, ctr_cap)
    nsect = min(src_cap, ctr_cap) // SECTOR_SIZE
    skip_set = skip_lbas if skip_lbas else set()
    first_diff = None
    dirty = 0
    lba = 0
    while lba < nsect:
        n = min(CHUNK_SECTORS, nsect - lba)
        a = src_read(lba, n)
        b = ctr_read(lba, n)
        if a != b:
            # Narrow down to the first differing sector inside the chunk, so
            # the report names a sector rather than a 1 MiB offset.
            for i in range(n):
                s = lba + i
                if s in skip_set:
                    continue
                if a[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE] != \
                   b[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]:
                    first_diff = s
                    break
            if first_diff is None:
                # every differing sector was a guest-written one
                dirty += sum(1 for i in range(n) if (lba + i) in skip_set)
                lba += n
                continue
            break
        dirty += sum(1 for i in range(n) if (lba + i) in skip_set)
        lba += n
    if first_diff is not None:
        return 'STALE', 'first difference at LBA %d (payload offset 0x%X)' \
               % (first_diff, first_diff * SECTOR_SIZE)
    return 'OK', dirty


def main():
    stale = missing = too_small = 0
    for raw_path, vdi_path, skip_lbas in TARGETS:
        if not os.path.exists(raw_path):
            print('  %-26s SKIP  (raw image not built)' % raw_path)
            continue
        if not os.path.exists(vdi_path):
            print('  %-26s MISSING  (create the VM disk once, then make vbox-sync)' % vdi_path)
            missing += 1
            continue
        try:
            status, detail = compare(raw_path, vdi_path, skip_lbas)
        except ValueError as e:
            print('  %-26s UNREADABLE  %s' % (vdi_path, e))
            missing += 1
            continue
        if status == 'TOO SMALL':
            print('  %-26s TOO SMALL  %s' % (vdi_path, detail))
            too_small += 1
            continue
        if status == 'OK':
            extra = '  [%d guest-written sector(s) ignored]' % detail if detail else ''
            print('  %-26s OK    (payload matches %s)%s' % (vdi_path, raw_path, extra))
        else:
            print('  %-26s STALE (%s)' % (vdi_path, detail))
            stale += 1

    if too_small:
        print('\nRESULT: CAPACITY TOO SMALL -- recreate the VM disk larger')
        return 3
    if missing:
        print('\nRESULT: MISSING -- cannot verify')
        return 2
    if stale:
        print('\nRESULT: STALE -- the VM would boot an older build.')
        print('        Run on the Windows host:  pwsh -NoProfile -File tools\\vbox_sync.ps1')
        return 1
    print('\nRESULT: IN SYNC -- the VM boots the current build')
    return 0


if __name__ == '__main__':
    sys.exit(main())
