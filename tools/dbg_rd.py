#!/usr/bin/env python3
"""One-shot diagnostic for the remote-desktop HTTP path: does QEMU actually
listen on the hostfwd port, and does the guest kernel receive the GET?"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = "build/os_textboot.img"
WORK = "build/remotedesktop_dbg.img"
LOG = "build/serial_dbg_rd.log"
MON = 4459
HTTPPORT = 18080
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash', '-': 'minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


subprocess.run("taskkill /F /IM qemu-system-x86_64.exe", shell=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)

import shutil
shutil.copy(IMG, WORK)
if os.path.exists(LOG):
    try:
        os.remove(LOG)
    except OSError:
        open(LOG, "w").close()

qemu = subprocess.Popen([
    QEMU, "-machine", "pc",
    "-drive", f"format=raw,file={WORK},if=ide",
    "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none",
    "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
    "-net", "nic,model=ne2k_isa",
    "-net", f"user,hostfwd=tcp::{HTTPPORT}-:8080",
    "-chardev", f"file,id=ser,path={LOG}",
    "-serial", "chardev:ser",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    mon = wait_sock(MON)
    mon.settimeout(3.0)
    try:
        mon.recv(65536)
    except Exception:
        pass
    time.sleep(8.0)
    type_line(mon, "root")
    type_line(mon, "admin")
    time.sleep(1.0)
    type_line(mon, "linux mc_launcher")
    time.sleep(2.0)
    for _ in range(40):
        if os.path.exists(LOG) and b"MC_LAUNCHER: ready" in open(LOG, "rb").read():
            break
        time.sleep(0.3)
    print("=== mc_launcher ready ===")

    # 1) Is QEMU listening on the hostfwd port RIGHT NOW? (GBK-safe)
    print("=== netstat -ano | 18080 ===")
    r = subprocess.run("netstat -ano", shell=True, capture_output=True)
    if r.stdout:
        txt = r.stdout.decode("gbk", "ignore")
        hits = [l for l in txt.splitlines() if "18080" in l]
        print("\n".join(hits) if hits else "  (no listener on 18080)")
    else:
        print("  netstat returned nothing")

    # 2) Raw connect + GET, report exact error
    print("=== raw connect 127.0.0.1:%d ===" % HTTPPORT)
    s = socket.socket()
    s.settimeout(3.0)
    try:
        s.connect(("127.0.0.1", HTTPPORT))
        print("  CONNECT OK")
        s.sendall(b"GET /desktop HTTP/1.0\r\n\r\n")
        time.sleep(1.0)
        try:
            data = s.recv(4096)
            print("  RECV %d bytes:" % len(data), data[:120])
        except Exception as e:
            print("  RECV ERR:", repr(e))
    except Exception as e:
        print("  CONNECT FAIL:", type(e).__name__, repr(e))
    s.close()
    time.sleep(1.0)

    # 3) Kernel log markers
    print("=== kernel serial markers ===")
    if os.path.exists(LOG):
        txt = open(LOG, "rb").read().decode("latin-1", "ignore")
        for tag in ("[HTTP]", "[NET]", "[NIC]", "NET:", "MC_LAUNCHER"):
            lines = [l for l in txt.splitlines() if tag in l]
            if lines:
                print("-- %s (%d lines) --" % (tag, len(lines)))
                print("\n".join(lines[:20]))
    mon.sendall(b"quit\n")
    time.sleep(1.0)
finally:
    try:
        qemu.wait(timeout=5.0)
    except Exception:
        qemu.terminate()
        qemu.wait(timeout=3.0)
