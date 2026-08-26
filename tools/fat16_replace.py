#!/usr/bin/env python3
"""
fat16_replace.py - Replace the contents of a file inside a FAT16 volume,
preserving the directory tree, BPB, and FAT structure.  Extends or truncates
the cluster chain as needed.  Both FAT copies are kept in sync.

Usage:
    python3 fat16_replace.py <image> <fat_path> <new_file> [--offset BYTES]
                             [--out OUTFILE]

  <image>     disk/partition image containing a FAT16 volume
  <fat_path>  absolute path inside the volume, e.g. /EFI/BOOT/BOOTX64.EFI
  <new_file>  file whose bytes replace the target
  --offset    byte offset of the FAT16 volume inside <image> (default 0)
  --out       write to a new file instead of overwriting <image>

The script round-trips the replacement (reads back the file bytes and
compares to <new_file>) before exiting 0, so a mismatch aborts with !=0.
"""
import struct
import sys
import os

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def short_name(path_part):
    """'BOOTX64.EFI' -> b'BOOTX64 EFI' (11 bytes, space padded)."""
    name, _, ext = path_part.upper().partition('.')
    name = name[:8].ljust(8, ' ')
    ext = ext[:3].ljust(3, ' ')
    return (name + ext).encode('ascii')

def parse_bpb(data, off):
    bps = u16(data, off + 11)
    spc = data[off + 13]
    res = u16(data, off + 14)
    nfats = data[off + 16]
    rootent = u16(data, off + 17)
    fatsz = u16(data, off + 22)
    # FAT16 if fatsz != 0
    root_bytes = rootent * 32
    fat_off = off + res * bps
    root_off = fat_off + nfats * fatsz * bps
    data_off = root_off + root_bytes
    return dict(bps=bps, spc=spc, res=res, nfats=nfats, rootent=rootent,
                fatsz=fatsz, fat_off=fat_off, root_off=root_off, data_off=data_off)

def fat_get(img, fat_off, c):
    return u16(img, fat_off + c * 2)

def fat_set(img, fat_off, nfats, fatsz, bps, c, val):
    for i in range(nfats):
        o = fat_off + i * fatsz * bps + c * 2
        struct.pack_into('<H', img, o, val & 0xFFFF)

def cluster_off(v, c):
    return v['data_off'] + (c - 2) * v['spc'] * v['bps']

def read_chain(img, v, start):
    chain = []
    c = start
    seen = set()
    while 2 <= c < 0xFFF7 and c not in seen:
        seen.add(c)
        chain.append(c)
        c = fat_get(img, v['fat_off'], c)
    return chain

def read_file(img, v, start, size):
    chain = read_chain(img, v, start)
    out = bytearray()
    per = v['spc'] * v['bps']
    for c in chain:
        o = cluster_off(v, c)
        out += img[o:o + per]
        if len(out) >= size:
            break
    return bytes(out[:size])

def find_free_cluster(img, v, after):
    total = (len(img) - v['data_off']) // (v['spc'] * v['bps']) + 2
    for c in range(max(2, after), total):
        if fat_get(img, v['fat_off'], c) == 0:
            return c
    return None

def write_file(img, v, start, data):
    """Overwrite file data starting at cluster `start`, extending/truncating
    the chain.  Returns the (possibly new) last cluster."""
    per = v['spc'] * v['bps']
    chain = read_chain(img, v, start)
    need = (len(data) + per - 1) // per
    # Extend chain if needed
    while len(chain) < need:
        last = chain[-1]
        nxt = find_free_cluster(img, v, last + 1)
        if nxt is None:
            raise RuntimeError("no free cluster to extend file")
        fat_set(img, v['fat_off'], v['nfats'], v['fatsz'], v['bps'], last, nxt)
        chain.append(nxt)
    # Truncate chain if file shrank
    if len(chain) > need:
        extra = chain[need:]
        for c in extra:
            fat_set(img, v['fat_off'], v['nfats'], v['fatsz'], v['bps'], c, 0)
        chain = chain[:need]
    # Write data
    for i, c in enumerate(chain):
        o = cluster_off(v, c)
        chunk = data[i * per:(i + 1) * per]
        img[o:o + len(chunk)] = chunk
        # zero the remainder of the last partial cluster
        if i == len(chain) - 1 and len(chunk) < per:
            img[o + len(chunk):o + per] = b'\x00' * (per - len(chunk))
    # Mark EOF on last cluster
    fat_set(img, v['fat_off'], v['nfats'], v['fatsz'], v['bps'], chain[-1], 0xFFFF)
    return chain[-1]

def read_dir_entries(img, v, first_cluster):
    """Yield (name11, attr, first_cluster, size, entry_offset) for a dir.
    Root directory (first_cluster==0) is the fixed-size root region."""
    per = v['spc'] * v['bps']
    if first_cluster == 0:
        base = v['root_off']
        n = v['rootent']
        for i in range(n):
            o = base + i * 32
            yield from_entry(img, o)
    else:
        chain = read_chain(img, v, first_cluster)
        for c in chain:
            o = cluster_off(v, c)
            for i in range(per // 32):
                yield from_entry(img, o + i * 32)

def from_entry(img, o):
    sig = img[o]
    if sig == 0x00:
        return  # end of dir
    if sig == 0xE5 or sig == 0x0F:
        return  # deleted / LFN
    name = bytes(img[o:o + 11])
    attr = img[o + 11]
    fc = u16(img, o + 26)
    size = u32(img, o + 28)
    return (name, attr, fc, size, o)

def find_entry(img, v, path_parts):
    """Navigate path_parts; return (dir_entry_offset, first_cluster, size) of
    the final component, plus the dir cluster to write back size/fc."""
    # start at root
    cur_cluster = 0  # root
    parent_cluster_for_write = None
    for depth, part in enumerate(path_parts):
        target = short_name(part)
        found = None
        for ent in read_dir_entries(img, v, cur_cluster):
            if ent is None:
                continue
            name, attr, fc, size, eo = ent
            if name == target:
                found = ent
                break
        if found is None:
            raise FileNotFoundError("not found: " + '/'.join(path_parts[:depth+1]))
        name, attr, fc, size, eo = found
        if depth == len(path_parts) - 1:
            return (eo, fc, size)
        # descend into subdirectory
        cur_cluster = fc

def main():
    args = sys.argv[1:]
    image = args[0]; fat_path = args[1]; new_file = args[2]
    offset = 0; out = None
    i = 3
    while i < len(args):
        if args[i] == '--offset':
            offset = int(args[i+1], 0); i += 2
        elif args[i] == '--out':
            out = args[i+1]; i += 2
        else:
            i += 1
    with open(image, 'rb') as f:
        img = bytearray(f.read())
    with open(new_file, 'rb') as f:
        data = f.read()
    v = parse_bpb(img, offset)
    print(f"[FAT16] bps={v['bps']} spc={v['spc']} res={v['res']} "
          f"nfats={v['nfats']} fatsz={v['fatsz']} rootent={v['rootent']}")
    parts = [p for p in fat_path.split('/') if p]
    eo, fc, old_size = find_entry(img, v, parts)
    print(f"[replace] {fat_path} old_size={old_size} new_size={len(data)} "
          f"first_cluster={fc}")
    write_file(img, v, fc, data)
    # update directory entry size
    struct.pack_into('<I', img, eo + 28, len(data))
    # round-trip verify
    got = read_file(img, v, fc, len(data))
    if got != data:
        # find first diff
        d = next((k for k in range(len(data)) if got[k] != data[k]), None)
        raise RuntimeError(f"round-trip mismatch at byte {d}")
    print("[verify] round-trip OK")
    if out:
        with open(out, 'wb') as f:
            f.write(img)
        print(f"[written] {out} ({len(img)} bytes)")
    else:
        with open(image, 'wb') as f:
            f.write(img)
        print(f"[written] {image} ({len(img)} bytes)")

if __name__ == '__main__':
    main()
