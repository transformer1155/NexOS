#!/usr/bin/env python3
"""Verify the Milestone: an in-guest Python interpreter (loaded via the Linux
compat layer) AUTHORS a 'hello.py' program and SUCCESSFULLY RUNS it.

Drives build/os_textboot.img under QEMU (pc/2G/tcg) and runs:
    linux python hello_demo.py
which loads the freestanding ELF32 `python` from the Linux partition, opens
the agent-authored `hello_demo.py` from the main SFS, and:
    - writes a real program file (hello.py) into its in-memory FS
    - exec()s that program
If the line  "Hello world from NexOS Linux + Python"  appears AND the guest
exits cleanly, the milestone passes.

Usage: verify_python_hello.py [wait_seconds]
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
SERIAL = os.path.join(BUILD, "serial_python_hello.txt")
QERR = os.path.join(BUILD, "qemu_python_hello.err")
MON_PORT = 5596

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
    QEMU, "-machine", "pc", "-m", "512", "-accel", "tcg",
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


def type_keys(s, text):
    for ch in text:
        # QEMU's `sendkey` has no literal "_" name; shift-minus produces it.
        key = {" ": "spc", ".": "dot", "-": "minus", "_": "shift-minus",
               "'": "apostrophe"}.get(ch, ch)
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)


drain()
print(f"[*] waiting for login prompt ({WAIT}s)...")
deadline = time.time() + WAIT
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

print("[*] waiting for shell...")
deadline = time.time() + WAIT
shell_ready = False
while time.time() < deadline:
    if os.path.exists(SERIAL):
        with open(SERIAL, "r", errors="replace") as fh:
            if "Shell ready" in fh.read():
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
type_keys(s, "linux python hello_demo.py")
s.sendall(b"sendkey ret\n")
time.sleep(4)
drain()

print("[*] waiting for execution result (25s)...")
time.sleep(25)
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
for marker in (
    "[python] NexOS Linux + Python interpreter online",
    "[python] source: file 'hello_demo.py'",
    "[python] wrote program hello.py:",
    "Hello world from NexOS Linux + Python",
    "[python] running hello.py ...",
    "[python] done.",
):
    print(f"  {'FOUND ' if marker in serial else 'MISS  '} {marker}")

tail = serial.splitlines()
for i, ln in enumerate(tail):
    if "linux" in ln.lower() and ("loading" in ln.lower() or "Launching" in ln.lower()):
        for j in range(max(0, i - 1), min(len(tail), i + 14)):
            print(f"  {tail[j].strip()[:120]}")
        print("  ...")
        break

if "linux: process exited" in serial:
    print("  guest exited: YES (clean return to cmd_linux)")
else:
    print("  guest exited: NO")

print("\n================ VERDICT ================")
# The milestone is *faithful* only if the AGENT-AUTHORED script at
# sfs_files/hello_demo.py was actually loaded and executed (not the
# built-in fallback demo).  Require that marker explicitly.
file_source = "[python] source: file 'hello_demo.py'" in serial
ran_hello  = "Hello world from NexOS Linux + Python" in serial
clean_exit = "linux: process exited" in serial
passed = file_source and ran_hello and clean_exit
if passed:
    print("  PASS: agent-authored hello_demo.py was LOADED and RAN inside the")
    print("        in-guest Python; it authored hello.py and executed it; clean exit.")
else:
    print("  FAIL: see MISS markers above.")
    if not file_source:
        print("        (the agent file was NOT loaded -- fell back to built-in demo)")
print(f"\nserial: {SERIAL}")
sys.exit(0 if passed else 1)
