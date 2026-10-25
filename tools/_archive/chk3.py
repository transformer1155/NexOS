#!/usr/bin/env python3
import struct
CAP = "build/cap.pcap"
data = open(CAP, "rb").read()
magic, = struct.unpack_from("<I", data, 0)
bo = "<" if magic == 0xa1b2c3d4 else ">"
off = 24
guest_tcp = None
iphdr = None
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from(f"{bo}IIII", data, off)
    off += 16
    pkt = data[off:off+incl]; off += incl
    if len(pkt) < 14: continue
    if struct.unpack_from(">H", pkt, 12)[0] != 0x0800: continue
    ip = pkt[14:]; ihl = (ip[0] & 0x0F) * 4; total_len, = struct.unpack_from(">H", ip, 2)
    if ip[9] != 6: continue
    tcp = ip[ihl:total_len]
    if ".".join(str(b) for b in ip[12:16]) == "10.0.2.15":
        guest_tcp = tcp; iphdr = ip; break
assert guest_tcp is not None
raw = bytes(guest_tcp)               # TCP segment (network order on wire)
sip = struct.unpack_from(">I", iphdr, 12)[0]   # network-order IPs
dip = struct.unpack_from(">I", iphdr, 16)[0]
print("sip=%08x dip=%08x" % (sip, dip))

seg0 = raw[:16] + b"\x00\x00" + raw[18:]   # cksum field zeroed
tcplen = len(seg0)
print("tcplen", tcplen)

def fold(s):
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return s & 0xFFFF

# ---- BIG-ENDIAN (RFC) algorithm ----
pseudo_be = struct.pack(">IIBBH", sip, dip, 0, 6, tcplen)
buf = pseudo_be + seg0
s = 0
for i in range(0, len(buf)-1, 2): s += struct.unpack_from(">H", buf, i)[0]
if len(buf)&1: s += buf[-1]<<8
V_be = (~fold(s)) & 0xFFFF
print("BIG-ENDIAN reads  -> V(host)=%04x  wire=%04x" % (V_be, ((V_be&0xFF)<<8)|(V_be>>8)))

# ---- LITTLE-ENDIAN reads algorithm (host-order IP split + uint16_t* read) ----
# pseudo from host-order IPs:
OUR = 0x0A00020F; DST = 0x0A000202
s = 0
s += (OUR>>16)&0xFFFF; s += OUR&0xFFFF
s += (DST>>16)&0xFFFF; s += DST&0xFFFF
s += 0x0006; s += tcplen
for i in range(0, tcplen-1, 2): s += struct.unpack_from("<H", seg0, i)[0]
if tcplen&1: s += seg0[-1]<<8
V_le = (~fold(s)) & 0xFFFF
print("LITTLE-ENDIAN reads -> V(host)=%04x  wire=%04x" % (V_le, ((V_le&0xFF)<<8)|(V_le>>8)))

# ---- empirical correct V from observed validation ----
def validate(cksum_host):
    b = bytes([cksum_host & 0xFF, (cksum_host >> 8) & 0xFF])
    frame = pseudo_be + raw[:16] + b + raw[18:]
    t = 0
    for i in range(0, len(frame)-1, 2): t += struct.unpack_from(">H", frame, i)[0]
    if len(frame)&1: t += frame[-1]<<8
    return fold(t)
stored = struct.unpack_from(">H", raw, 16)[0]
W = validate(stored)
se = fold((W - ((stored&0xFF)<<8|(stored>>8))) & 0xFFFF)
correct_read = (~se) & 0xFFFF
correct_V = ((correct_read & 0xFF) << 8) | (correct_read >> 8)
print("EMPIRICAL correct V(host)=%04x  wire=%04x  validate=%04x" % (correct_V, correct_read, validate(correct_V)))
