#!/usr/bin/env python3
"""Isolation test: guest does NOT send SYN-ACK, so the host keeps
retransmitting SYN.  Count how many inbound frames the guest kernel
actually receives (RX packet lines in the serial log).  If RX stays > 1
without any guest transmit, the NIC receive path is fine and the fault is
the guest's own transmit wedging the ring.  If RX == 1, the ring dies
after the first packet regardless of transmit."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
WORK = "build/remotedesktop_iso.img"
LOG = "build/serial_iso.log"
MON = 4461
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
if os.path.exists(LOG): os.remove(LOG)

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
    print("=== mc_launcher ready; now hammering connect (expect retransmitted SYNs) ===")
    # Repeatedly connect with short timeouts so the host keeps sending SYNs.
    for i in range(8):
        s = socket.socket(); s.settimeout(1.2)
        try:
            s.connect(("127.0.0.1", HTTPPORT))
            print(f"  attempt {i}: CONNECT OK (unexpected)")
            s.sendall(b"GET /screen HTTP/1.0\r\n\r\n")
        except Exception as e:
            print(f"  attempt {i}: {type(e).__name__}")
        s.close()
        time.sleep(0.3)
    time.sleep(3.0)
    # Count RX frames in the serial log
    txt = open(LOG, "rb").read().decode("latin-1", "ignore")
    lines = txt.splitlines()
    rx = [l for l in lines if "[NP] RX packet len=" in l]
    syn = [l for l in lines if "[TCP] SYN recv" in l]
    nic = [l for l in lines if "[NIC] bnry=" in l]
    print(f"=== RX packets: {len(rx)} ===")
    for l in rx[:15]: print("  ", l[:60])
    print(f"=== TCP SYN recv: {len(syn)} ===")
    print(f"=== NIC ring samples (last 3): {len(nic)} ===")
    for l in nic[-3:]: print("  ", l[:60])
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try: qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate(); qemu.wait(timeout=3.0)
