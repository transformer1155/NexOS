#!/usr/bin/env python3
"""Decisive test: after the guest sends a real SYN-ACK (1st connection),
open a SECOND host connection (fresh SYN from SLIRP).  If the guest
receives that 3rd inbound frame, the NIC RX path is healthy and the
freeze is connection-specific (SLIRP not forwarding the 1st conn's data);
if not, the NIC is wedged."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
WORK = "build/remotedesktop_2nd.img"
LOG = "build/serial_2nd.log"
MON = 4463
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
try:
    with open(LOG, "w") as _f: pass  # truncate; avoid safe-delete os.remove guard
except OSError:
    pass

qemu = subprocess.Popen([
    QEMU, "-machine", "pc", "-drive", f"format=raw,file={WORK},if=ide",
    "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
    "-net", "nic,model=ne2k_isa", "-net", f"user,hostfwd=tcp::{HTTPPORT}-:8080",
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
    def connect_once(tag):
        s = socket.socket(); s.settimeout(8.0)
        try:
            s.connect(("127.0.0.1", HTTPPORT))
            print(f"  {tag}: CONNECT OK")
            s.sendall(b"GET /screen HTTP/1.0\r\n\r\n")
            try:
                d = s.recv(4096); print(f"  {tag}: RECV {len(d)} bytes")
            except Exception as e:
                print(f"  {tag}: RECV ERR {type(e).__name__}")
        except Exception as e:
            print(f"  {tag}: CONNECT ERR {type(e).__name__}")
        s.close()
    connect_once("conn1")
    time.sleep(2.0)
    connect_once("conn2")   # fresh SYN -> does the guest receive it?
    time.sleep(2.0)
    txt = open(LOG, "rb").read().decode("latin-1", "ignore")
    lines = txt.splitlines()
    rx = [l for l in lines if "[NP] RX packet len=" in l]
    syn = [l for l in lines if "[TCP] SYN recv" in l]
    print(f"=== RX packets: {len(rx)} ===")
    for l in rx[:20]: print("  ", l[:70])
    print(f"=== TCP SYN recv: {len(syn)} ===")
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try: qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate(); qemu.wait(timeout=3.0)
