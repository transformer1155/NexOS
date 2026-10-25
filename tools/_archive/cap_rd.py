#!/usr/bin/env python3
"""Capture the real wire frames between guest (10.0.2.15) and SLIRP (10.0.2.2)
using QEMU's -net dump, then independently verify the guest's SYN-ACK
TCP and IP checksums.  Ground truth, no guessing."""
import os, sys, socket, time, subprocess, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
WORK = "build/remotedesktop_cap.img"
LOG = "build/serial_cap.log"
CAP = "build/cap.pcap"
MON = 4464
HTTPPORT = 18080
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

def type_line(mon, s):
    km = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash', '-': 'minus'}
    for ch in s:
        k = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else km.get(ch, ch)
        mon.sendall(f"sendkey {k}\n".encode()); time.sleep(0.08)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.4)

def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try: return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError: time.sleep(0.2)
    raise RuntimeError("monitor not ready")

subprocess.run("taskkill /F /IM qemu-system-x86_64.exe", shell=True,
                stdout=subprocess.DEVDEVNULL if False else subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
time.sleep(2)
import shutil
shutil.copy(IMG, WORK)
for f in (LOG, CAP):
    try:
        with open(f, "w") as _f: pass
    except OSError:
        pass

qemu = subprocess.Popen([
    QEMU, "-machine", "pc", "-drive", f"format=raw,file={WORK},if=ide",
    "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
    "-netdev", f"user,id=net0,hostfwd=tcp::{HTTPPORT}-:8080",
    "-device", "ne2k_isa,netdev=net0",
    "-object", f"filter-dump,id=dump0,netdev=net0,file={CAP}",
    "-chardev", f"file,id=ser,path={LOG}", "-serial", "chardev:ser",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    mon = wait_sock(MON); mon.settimeout(3.0)
    try: mon.recv(65536)
    except Exception: pass
    time.sleep(8.0)
    type_line(mon, "root"); type_line(mon, "admin"); time.sleep(1.0)
    type_line(mon, "linux mc_launcher"); time.sleep(2.0)
    for _ in range(40):
        if os.path.exists(LOG) and b"MC_LAUNCHER: ready" in open(LOG, "rb").read():
            break
        time.sleep(0.3)
    print("=== mc_launcher ready ===")
    # single connect + GET, hold open briefly
    s = socket.socket(); s.settimeout(2.0)
    try:
        s.connect(("127.0.0.1", HTTPPORT))
        print("CONNECT OK")
        s.sendall(b"GET /screen HTTP/1.0\r\n\r\n")
        time.sleep(2.0)
    except Exception as e:
        print("CONNECT ERR", type(e).__name__)
    s.close()
    time.sleep(1.0)
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try: qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate(); qemu.wait(timeout=3.0)

# ---- analyze pcap ----
print("=== analyzing", CAP, "===")
if not os.path.exists(CAP):
    print("NO CAP FILE"); sys.exit(1)
data = open(CAP, "rb").read()
magic, = struct.unpack_from("<I", data, 0)
if magic == 0xa1b2c3d4:
    bo = "<"; ver = 2
elif magic == 0xd4c3b2a1:
    bo = ">"; ver = 2
else:
    print("bad pcap magic", hex(magic)); sys.exit(1)
off = 24
def ip2s(b): return f"{b[0]}.{b[1]}.{b[2]}.{b[3]}"
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from(f"{bo}IIII", data, off)
    off += 16
    pkt = data[off:off+incl]; off += incl
    if len(pkt) < 14: continue
    eth_type, = struct.unpack_from(">H", pkt, 12)
    if eth_type != 0x0800: continue
    ip = pkt[14:]
    if len(ip) < 20: continue
    ver_ihl = ip[0]; ihl = (ver_ihl & 0x0F) * 4
    total_len, = struct.unpack_from(">H", ip, 2)
    proto = ip[9]
    src = ip2s(ip[12:16]); dst = ip2s(ip[16:20])
    # IP checksum validation: sum over header (incl. stored cksum) must be 0xFFFF
    def cksum_validate(buf):
        s = 0
        for i in range(0, len(buf)-1, 2):
            s += struct.unpack_from(">H", buf, i)[0]
        if len(buf) & 1:
            s += buf[-1] << 8
        while s >> 16:
            s = (s & 0xFFFF) + (s >> 16)
        return s & 0xFFFF
    ip_calc = cksum_validate(ip[:ihl])
    ip_actual, = struct.unpack_from(">H", ip, 10)
    if proto != 6: continue
    tcp = ip[ihl:total_len]
    if len(tcp) < 20: continue
    sport, dport, seq, ack = struct.unpack_from(">HHII", tcp, 0)
    off_flags, = struct.unpack_from(">H", tcp, 12)
    tcp_off = (off_flags >> 12) * 4
    flags = off_flags & 0x01FF
    fl_s = ""
    if flags & 0x02: fl_s += "SYN"
    if flags & 0x10: fl_s += "+ACK"
    if flags & 0x01: fl_s += "+FIN"
    if flags & 0x08: fl_s += "+PSH"
    # TCP checksum validation over pseudo-header + segment (incl. stored cksum)
    pseudo = struct.pack(">IIBBH",
                         struct.unpack_from(">I", ip, 12)[0],
                         struct.unpack_from(">I", ip, 16)[0],
                         0, 6, total_len - ihl)
    tcp_calc = cksum_validate(pseudo + tcp)
    tcp_actual, = struct.unpack_from(">H", tcp, 16)
    direction = f"{src}:{sport}->{dst}:{dport}"
    tag = "GUEST->" if src == "10.0.2.15" else "HOST->"
    print(f"{tag}{direction} {fl_s} seq={seq} ack={ack} "
          f"ip_cksum={'OK' if ip_calc==0xFFFF else 'BAD(sum=%04x)'%ip_calc} "
          f"tcp_cksum={'OK' if tcp_calc==0xFFFF else 'BAD(sum=%04x,stored=%04x)'%(tcp_calc,tcp_actual)}")
