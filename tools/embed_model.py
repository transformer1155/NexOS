#!/usr/bin/env python3
"""Embed a GGUF model into a raw disk image as a bare LBA blob.

SFS caps out around 384 KiB, so real weights cannot live in the filesystem.
Instead the build drops the file straight onto the disk after the SFS region:

    LBA 4095   one-sector descriptor: "MINIMDL1" + u64 size + u64 start_lba
    LBA 4096+  raw GGUF payload

The 64-bit kernel's `model load` reads the descriptor, big_alloc()s the
payload and hands it to qwen_load().

Usage: embed_model.py <image> <model.gguf> [hdr_lba] [data_lba]
"""
import os
import struct
import sys

MAGIC = b"MINIMDL1"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    img = sys.argv[1]
    model = sys.argv[2]
    hdr_lba = int(sys.argv[3]) if len(sys.argv) > 3 else 4095
    data_lba = int(sys.argv[4]) if len(sys.argv) > 4 else 4096

    if not os.path.exists(img):
        print("embed_model: no such image %s" % img, file=sys.stderr)
        return 1
    if not os.path.exists(model):
        print("embed_model: no such model %s" % model, file=sys.stderr)
        return 1

    size = os.path.getsize(model)
    sectors = (size + 511) // 512
    need = (data_lba + sectors) * 512

    with open(img, "r+b") as f:
        f.seek(0, os.SEEK_END)
        if f.tell() < need:
            f.truncate(need)

        hdr = bytearray(512)
        hdr[0:8] = MAGIC
        struct.pack_into("<QQ", hdr, 8, size, data_lba)
        f.seek(hdr_lba * 512)
        f.write(hdr)

        f.seek(data_lba * 512)
        with open(model, "rb") as m:
            while True:
                chunk = m.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)

    print("    Model:    %d bytes (%d sectors) at LBA %d, descriptor at LBA %d"
          % (size, sectors, data_lba, hdr_lba))
    return 0


if __name__ == "__main__":
    sys.exit(main())
