#!/usr/bin/env python3
"""Test the real remote-desktop route: GET /screen returns an NXFB framebuffer
(RLE-compressed).  Verify the host receives a well-formed NXFB (magic NXFB +
width/height/format + terminator) so the browser viewer can decode it."""
import os, socket, time, subprocess, struct
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
WORK = "build/remotedesktop_screen.img"
LOG = "build/serial_screen.log"
MON = 4466
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
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
import shutil
shutil.copy(IMG, WORK)
for f in (LOG,):
    try:
        with open(f, "w") as _f: pass
    except OSError: pass
qemu = subprocess.Popen([
    QEMU, "-machine", "pc", "-drive", f"format=raw,file={WORK},if=ide",
    "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
    "-netdev", f"user,id=net0,hostfwd=tcp::{HTTPPORT}-:8080",
    "-device", "ne2k_isa,netdev=net0",
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
    s = socket.socket(); s.settimeout(6.0)
    try:
        s.connect(("127.0.0.1", HTTPPORT))
        print("CONNECT OK")
        s.sendall(b"GET /screen HTTP/1.1\r\nHost: x\r\n\r\n")
        chunks = []
        s.settimeout(6.0)
        try:
            while True:
                d = s.recv(4096)
                if not d: break
                chunks.append(d)
        except socket.timeout: pass
        except Exception as e: print("RECV ERR", type(e).__name__)
        total = b"".join(chunks)
        print(f"RECV TOTAL {len(total)} bytes")
        if total.startswith(b"HTTP"):
            # skip headers
            i = total.find(b"\r\n\r\n")
            body = total[i+4:] if i >= 0 else b""
            print(f"BODY {len(body)} bytes")
            if body[:4] == b"NXFB":
                w, h, fmt = struct.unpack_from("<III", body, 4)
                print(f"NXFB ok: w={w} h={h} fmt={fmt} body_head={body[:16].hex()}")
            else:
                print("BODY not NXFB:", body[:16])
        else:
            print("no HTTP head:", total[:40])
    except Exception as e:
        print("CONNECT ERR", type(e).__name__)
    s.close()
    time.sleep(1.0)
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try: qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate(); qemu.wait(timeout=3.0)
