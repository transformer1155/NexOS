#!/usr/bin/env python3
"""
vbox_check.py - Report whether the VirtualBox disk containers still match the
raw images this repo builds.  Read-only; safe on any platform; needs no
VBoxManage.

Why a *payload* comparison and not a file hash
----------------------------------------------
build/os_v2_vbox.vdi is a real VirtualBox VDI container (dynamic, 1 MiB
blocks, signature 0xBEDA107F at offset 0x40), so its bytes are nothing like
build/os_v2.img.  Comparing the two files directly would always report
"different" and teach the reader to ignore the warning.  So this script
extracts the VDI payload and compares it against the raw image byte for byte,
which is the only comparison that answers the question actually being asked:
"would the VM boot the build I just made?"

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
GUEST_WRITABLE_LBAS = {1500, 2047}

# (source, container, source_is_vdi).  os_v2.img is a raw disk image; data.vdi
# is ALREADY a VDI container (tools/make_data_vdi.py writes one), so comparing
# it byte-for-byte against another VDI's payload would compare two container
# files and report a permanent false "stale".  Compare payloads in that case.
TARGETS = [
    ('build/os_v2.img', 'build/os_v2_vbox.vdi', False),
    ('build/data.vdi',  'build/data_vbox.vdi',  True),
]


def vdi_payload(path):
    """Return (payload bytes, capacity bytes) from a VDI container."""
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) < 0x200:
        raise ValueError('%s: too small to be a VDI' % path)
    sig = struct.unpack_from('<I', raw, HDR_SIG_OFF)[0]
    if sig != VDI_SIGNATURE:
        raise ValueError('%s: not a VDI container (signature=%08x)' % (path, sig))
    image_type, = struct.unpack_from('<I', raw, 0x4C)
    off_blocks, = struct.unpack_from('<I', raw, 0x154)
    off_data,   = struct.unpack_from('<I', raw, 0x158)
    cb_disk,    = struct.unpack_from('<Q', raw, 0x170)
    if image_type == VDI_TYPE_STATIC:
        return raw[off_data:off_data + cb_disk], cb_disk
    if image_type == VDI_TYPE_DYNAMIC:
        c_blocks, = struct.unpack_from('<I', raw, 0x180)
        out = bytearray(cb_disk)
        for b in range(c_blocks):
            ent, = struct.unpack_from('<I', raw, off_blocks + b * 4)
            if ent == 0xFFFFFFFF:
                continue
            # Block-table entries are BLOCK numbers, not sector numbers:
            # offset = offData + ent * BLOCK_SIZE.  Using SECTOR_SIZE here
            # (this file's first version) shifted every block but the first by
            # 1 MiB - 512 bytes and made a perfectly good container look stale
            # from block 1 onward (first difference exactly at 0x100000).
            src = off_data + ent * BLOCK_SIZE
            chunk = raw[src:src + BLOCK_SIZE]
            base = b * BLOCK_SIZE
            out[base:base + len(chunk)] = chunk
        return bytes(out), cb_disk
    raise ValueError('%s: unsupported VDI type %d' % (path, image_type))


def main():
    stale = missing = too_small = 0
    for raw_path, vdi_path, src_is_vdi in TARGETS:
        if not os.path.exists(raw_path):
            print('  %-26s SKIP  (raw image not built)' % raw_path)
            continue
        if not os.path.exists(vdi_path):
            print('  %-26s MISSING  (create the VM disk once, then make vbox-sync)' % vdi_path)
            missing += 1
            continue
        try:
            raw, _ = vdi_payload(raw_path) if src_is_vdi else (open(raw_path, 'rb').read(), None)
        except ValueError as e:
            print('  %-26s UNREADABLE  %s' % (raw_path, e))
            missing += 1
            continue
        try:
            payload, cap = vdi_payload(vdi_path)
        except ValueError as e:
            print('  %-26s UNREADABLE  %s' % (vdi_path, e))
            missing += 1
            continue
        if len(raw) > cap:
            print('  %-26s TOO SMALL  image %d > capacity %d bytes'
                  % (vdi_path, len(raw), cap))
            too_small += 1
            continue
        # Compare sector by sector, skipping the ones the guest legitimately
        # dirties at runtime (see GUEST_WRITABLE_LBAS).  Comparing raw bytes
        # directly would flag a container that simply ran once.
        n = min(len(raw), len(payload))
        first_diff = None
        skipped = []
        for lba in range(0, n // SECTOR_SIZE):
            off = lba * SECTOR_SIZE
            if lba in GUEST_WRITABLE_LBAS:
                if raw[off:off + SECTOR_SIZE] != payload[off:off + SECTOR_SIZE]:
                    skipped.append(lba)
                continue
            if raw[off:off + SECTOR_SIZE] != payload[off:off + SECTOR_SIZE]:
                first_diff = off
                break
        # any remaining tail bytes (not a whole sector) still matter
        if first_diff is None and n < len(raw):
            if raw[n:] != payload[n:]:
                first_diff = n
        tail_clean = payload[len(raw):].find(b'\x00') == 0 if len(payload) > len(raw) else True
        if first_diff is None and tail_clean:
            extra = ''
            if skipped:
                extra = '  [guest-written LBA %s ignored]' % ','.join(str(x) for x in skipped)
            print('  %-26s OK    (payload matches %s)%s' % (vdi_path, raw_path, extra))
        else:
            where = ('first difference at payload offset %d (0x%X, LBA %d)'
                     % (first_diff, first_diff, first_diff // SECTOR_SIZE)) \
                    if first_diff is not None else 'length only'
            print('  %-26s STALE (%s)' % (vdi_path, where))
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
