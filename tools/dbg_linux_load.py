#!/usr/bin/env python3
"""Focused debug: boot textboot, login, run `linux mc_launcher`, capture the
kernel [DBG] header dump (printed before the PH flood) and the first PH lines,
then exit.  Does NOT wait for HTTP."""
import os, sys, time, subprocess, socket

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
LOG = "build/serial_dbg_linux.log"
MONPORT = 4461
HTTPPORT = 18081
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

MON = f"tcp:127.0.0.1:{MONPORT}"
cmd = [
    QEMU, "-machine", "pc", "-m", "256M", "-accel", "tcg", "-vga", "std",
    "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MONPORT},server,nowait",
    "-net", "nic,model=ne2k_isa", "-net", f"user,hostfwd=tcp::{HTTPPORT}-:8080",
    "-chardev", f"file,id=ser,path={LOG}", "-serial", "chardev:ser",
    "-drive", f"file={IMG},format=raw,if=ide,index=0,media=disk",
]
print("launching", cmd)
p = subprocess.Popen(cmd)

def mon_connect():
    end = time.time() + 20
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", MONPORT), timeout=0.5)
            return s
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

mon = mon_connect()

def key(k):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(0.08)

def type_line(s):
    for ch in s:
        if 'A' <= ch <= 'Z':
            key(f"shift-{ch.lower()}")
        elif ch == ' ':
            key("spc")
        elif ch == '.':
            key("dot")
        elif ch == '/':
            key("slash")
        elif ch == '-':
            key("minus")
        elif ch == '_':
            key("shift-minus")
        else:
            key(ch)
    key("ret"); time.sleep(0.4)

time.sleep(3)
type_line("root")
type_line("admin")
time.sleep(1)
type_line("linux mc_launcher")
# let the kernel dump [DBG] + a few PH lines
time.sleep(8)
mon.sendall(b"quit\n")
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
print("=== captured DBG/PH lines ===")
with open(LOG) as f:
    for line in f:
        if "[DBG]" in line or "PH0 " in line or "PH1 " in line or "linux: loading" in line or "entry bytes" in line:
            print(line.rstrip())
print("=== done ===")
