#!/usr/bin/env python3
"""Baseline test: run `linux linuxhello` on the BIOS os.img via QEMU sendkey.

Verifies the existing Milestone-0 ELF32 loader end-to-end:
  - shell boots, `linux linuxhello` dispatches
  - ELF loads, entry runs, int 0x80 syscalls dispatch ([sc N] markers)
  - guest exits cleanly back into cmd_linux

Needs -machine pc (PIIX legacy IDE) for the ATA PIO reads to work.

Usage: verify_linux_hello.py [wait_seconds]
"""
import os
import socket
import subprocess
import sys
import time

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_linux_hello.txt")
QERR = os.path.join(BUILD, "qemu_linux_hello.err")
MON_PORT = 5594

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 60

def safe_remove(path):
    try:
        if os.path.exists(path):
            os.remove(path)
    except OSError:
        try:
            subprocess.run(["cmd", "/c", "del", "/f", "/q", path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

for f in (SERIAL, QERR):
    safe_remove(f)

cmd = [
    QEMU, "-machine", "pc", "-m", "2G", "-accel", "tcg",
    "-drive", f"if=ide,format=raw,file={DISK}",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std", "-display", "none",
]
print("[*] launching QEMU (pc/2G/tcg)...")
errf = open(QERR, "wb")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf)

s = None
for _ in range(20):
    if p.poll() is not None:
        errf.close()
        print(f"[FATAL] QEMU exited early rc={p.returncode}")
        with open(QERR, "r", errors="replace") as fh:
            print("  stderr:", fh.read().strip()[:400] or "(empty)")
        sys.exit(2)
    try:
        s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=3)
        break
    except OSError:
        time.sleep(0.5)
s.settimeout(2.0)


def drain():
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except OSError:
        pass
    return buf.decode(errors="replace")


drain()
print(f"[*] waiting for shell ({WAIT}s)...")
def type_keys(s, text):
    for ch in text:
        key = {" ": "spc", ".": "dot"}.get(ch, ch)
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)


# textboot drops to the security login prompt -> login as root/admin first
print("[*] waiting for login prompt...")
deadline = time.time() + 60
login_seen = False
while time.time() < deadline:
    if os.path.exists(SERIAL):
        with open(SERIAL, "r", errors="replace") as fh:
            if "login:" in fh.read():
                login_seen = True
                break
    time.sleep(1)
print(f"    login prompt: {login_seen}")
if login_seen:
    drain()
    type_keys(s, "root")
    s.sendall(b"sendkey ret\n")
    time.sleep(1)
    drain()
    type_keys(s, "admin")
    s.sendall(b"sendkey ret\n")
    time.sleep(3)
    drain()

# now wait for the shell, then run: linux linuxhello
print("[*] waiting for shell...")
deadline = time.time() + 60
shell_ready = False
while time.time() < deadline:
    if os.path.exists(SERIAL):
        with open(SERIAL, "r", errors="replace") as fh:
            txt = fh.read()
        if "Shell ready" in txt:
            shell_ready = True
            break
    time.sleep(1)
print(f"    shell ready: {shell_ready}")
if not shell_ready:
    s.sendall(b"quit\n")
    p.kill()
    errf.close()
    print("[FAIL] shell never became ready")
    sys.exit(1)

drain()
type_keys(s, "linux hello.nex")
s.sendall(b"sendkey ret\n")
time.sleep(4)
drain()

print("[*] waiting for execution result (20s)...")
time.sleep(20)
s.sendall(b"quit\n")
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
errf.close()

# ---- analyse ----
serial = ""
if os.path.exists(SERIAL):
    with open(SERIAL, "r", errors="replace") as fh:
        serial = fh.read()

print("\n================ EVIDENCE ================")
tail = serial.splitlines()
# find lines around the linux launch
for i, ln in enumerate(tail):
    if "linux" in ln.lower() and ("elf" in ln.lower() or "loading" in ln.lower() or "Launching" in ln):
        for j in range(max(0, i - 1), min(len(tail), i + 12)):
            print(f"  {tail[j].strip()[:110]}")
        print("  ...")
        break

import re
scs = re.findall(r"\[sc (\d+)\]", serial)
print(f"  syscall markers seen: {len(scs)} -> {scs[:40]}")
if "process exited" in serial:
    print("  guest exited: YES (clean return to cmd_linux)")
else:
    print("  guest exited: NO (no 'process exited' marker)")

print("\n================ VERDICT ================")
if "linux: loading" in serial and "process exited" in serial:
    print("  GOOD: ELF32 loaded and guest exited cleanly.")
elif "linux: loading" in serial:
    print("  PARTIAL: ELF loaded but no clean exit seen.")
else:
    print("  FAIL: linux command did not load the ELF.")
print(f"\nserial: {SERIAL}")
