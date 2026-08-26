#!/usr/bin/env python3
import struct
CAP = "build/cap.pcap"
data = open(CAP, "rb").read()
magic, = struct.unpack_from("<I", data, 0)
bo = "<" if magic == 0xa1b2c3d4 else ">"
off = 24
guest_tcp = None
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from(f"{bo}IIII", data, off)
    off += 16
    pkt = data[off:off+incl]; off += incl
    if len(pkt) < 14: continue
    if struct.unpack_from(">H", pkt, 12)[0] != 0x0800: continue
    ip = pkt[14:]; ihl = (ip[0] & 0x0F) * 4; total_len, = struct.unpack_from(">H", ip, 2)
    if ip[9] != 6: continue
    tcp = ip[ihl:total_len]
    src = ".".join(str(b) for b in ip[12:16])
    if src == "10.0.2.15":
        guest_tcp = tcp; break

assert guest_tcp is not None
raw = bytes(guest_tcp)
print("raw 20 tcp bytes:", raw[:20].hex())
# zero checksum field for computation
seg = bytearray(raw[:20])
seg[16] = 0; seg[17] = 0
print("seg (cksum zeroed):", bytes(seg).hex())

# ---- method 1: standard RFC (big-endian words over pseudo + segment) ----
def sum_be(buf):
    s = 0
    for i in range(0, len(buf)-1, 2):
        s += struct.unpack_from(">H", buf, i)[0]
    if len(buf) & 1:
        s += buf[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s & 0xFFFF

src_ip = struct.unpack_from(">I", raw, 12)[0]   # network order
dst_ip = struct.unpack_from(">I", raw, 16)[0]
pseudo = struct.pack(">IIBBH", src_ip, dst_ip, 0, 6, 20)
raw1 = sum_be(pseudo + bytes(seg))
m1 = (~raw1) & 0xFFFF
print(f"method1 (RFC big-endian) rawsum={raw1:04x} host-stored = {m1:04x}  (wire reads as {((m1&0xFF)<<8)|(m1>>8):04x})")

# ---- method 2: replicate C (host IP split + b[i]<<8|b[i+1]) ----
OUR = 0x0A00020F
DST = 0x0A000202
def c_rep(src, dst, seg, length):
    s = 0
    s += (src >> 16) & 0xFFFF
    s += src & 0xFFFF
    s += (dst >> 16) & 0xFFFF
    s += dst & 0xFFFF
    s += 0x0006
    s += length
    i = 0
    while length > 1:
        s += (seg[i] << 8) | seg[i+1]
        i += 2; length -= 2
    if length == 1:
        s += seg[i] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF
m2 = c_rep(OUR, DST, seg, 20)
# print raw sum inside c_rep by recomputing
s2 = 0
s2 += (OUR >> 16) & 0xFFFF; s2 += OUR & 0xFFFF
s2 += (DST >> 16) & 0xFFFF; s2 += DST & 0xFFFF
s2 += 0x0006; s2 += 20
ii = 0; ll = 20
while ll > 1:
    s2 += (seg[ii] << 8) | seg[ii+1]; ii += 2; ll -= 2
while s2 >> 16: s2 = (s2 & 0xFFFF) + (s2 >> 16)
print(f"method2 (C replica)          rawsum={s2:04x} host-stored = {m2:04x}  (wire reads as {((m2&0xFF)<<8)|(m2>>8):04x})")

# ---- wire validation (receiver view, big-endian, incl stored cksum) ----
stored, = struct.unpack_from(">H", raw, 16)
val = sum_be(pseudo + raw[:20])
print(f"wire validation sum (incl stored {stored:04x}) = {val:04x}  (want FFFF)")
