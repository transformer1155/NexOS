#!/usr/bin/env python3
"""Replicate the guest's tcp_checksum (host-order src/dst) against the
captured guest SYN-ACK wire bytes, and compute the CORRECT checksum
independently, to localize the bug."""
import struct

CAP = "build/cap.pcap"
data = open(CAP, "rb").read()
magic, = struct.unpack_from("<I", data, 0)
bo = "<" if magic == 0xa1b2c3d4 else ">"
off = 24
frames = []
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from(f"{bo}IIII", data, off)
    off += 16
    pkt = data[off:off+incl]; off += incl
    if len(pkt) < 14: continue
    eth_type, = struct.unpack_from(">H", pkt, 12)
    if eth_type != 0x0800: continue
    ip = pkt[14:]
    ihl = (ip[0] & 0x0F) * 4
    total_len, = struct.unpack_from(">H", ip, 2)
    proto = ip[9]
    if proto != 6: continue
    tcp = ip[ihl:total_len]
    sport, dport, seq, ack = struct.unpack_from(">HHII", tcp, 0)
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])
    if src != "10.0.2.15": continue  # guest frame
    frames.append((src, dst, sport, dport, tcp, total_len - ihl))
    break

assert frames, "no guest frame"
src, dst, sport, dport, tcp, tcplen = frames[0]
print(f"guest frame {src}:{sport} -> {dst}:{dport} tcplen={tcplen}")

# host-order IPs
def ip2host(s):
    a,b,c,d = (int(x) for x in s.split("."))
    return (a<<24)|(b<<16)|(c<<8)|d
OUR = ip2host("10.0.2.15")
DST = ip2host(dst)

# --- replicate guest tcp_checksum (host-order src/dst), byte-exact ---
def guest_tcp_checksum(src, dst, seg, length, proto_word=0x0006, len_mode="native"):
    s = 0
    s += (src >> 16) & 0xFFFF
    s += src & 0xFFFF
    s += (dst >> 16) & 0xFFFF
    s += dst & 0xFFFF
    s += proto_word
    s += (length if len_mode == "native" else ((length >> 8) | (length << 8)) & 0xFFFF)
    p = 0
    ln = length
    while ln > 1:
        s += struct.unpack_from("<H", seg, p)[0]   # uint16_t* read on little-endian
        p += 2; ln -= 2
    if ln == 1:
        s += seg[p] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

print(f"IP words: src=({OUR>>16:04x},{OUR&0xFFFF:04x}) dst=({DST>>16:04x},{DST&0xFFFF:04x})")
# zero the checksum field before replicating the guest computation
seg0 = tcp[0:16] + b"\x00\x00" + tcp[18:]
print("TCP segment words (little-endian read, cksum zeroed):", [f"{struct.unpack_from('<H', seg0, i)[0]:04x}" for i in range(0, tcplen, 2)])
guest_val = guest_tcp_checksum(OUR, DST, seg0, tcplen)
print(f"guest-style NEW (0x0006, native len)  = {guest_val:04x}")

def fixed_tcp_checksum(src, dst, seg, length):
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

fixed_val = fixed_tcp_checksum(OUR, DST, seg0, tcplen)
print(f"guest-style FIXED (big-endian reads)  = {fixed_val:04x}  (want a066)")
old_val = guest_tcp_checksum(OUR, DST, seg0, tcplen, proto_word=0x0600, len_mode="htons")
print(f"guest-style OLD (0x0600, htons len)   = {old_val:04x}")
stored, = struct.unpack_from(">H", tcp, 16)
print(f"stored checksum on wire               = {stored:04x}")

# --- CORRECT checksum: sum pseudo(network IPs) + segment(network bytes) ---
def cksum(buf):
    s = 0
    for i in range(0, len(buf)-1, 2):
        s += struct.unpack_from(">H", buf, i)[0]
    if len(buf) & 1:
        s += buf[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s & 0xFFFF

pseudo = struct.pack(">IIBBH", ip2host(src), ip2host(dst), 0, 6, tcplen)
correct = (~cksum(pseudo + tcp)) & 0xFFFF   # tcp already has cksum field; validate-style would be 0xFFFF
# but to get the value that should be STORED, zero the field first:
seg0 = tcp[0:16] + b"\x00\x00" + tcp[18:]
correct_stored = (~cksum(pseudo + seg0)) & 0xFFFF
print(f"CORRECT checksum that should be stored              = {correct_stored:04x}")
print(f"validation sum (pseudo+seg incl stored)             = {cksum(pseudo+tcp):04x} (want FFFF)")
