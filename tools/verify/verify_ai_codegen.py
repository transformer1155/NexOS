#!/usr/bin/env python3
"""Verify the new milestone: the OS's built-in AI engine AUTHORS a Python
program at runtime and the Linux-compat Python interpreter RUNS it.

Drives build/os_textboot.img under QEMU (pc/512M/tcg) and runs:
    ai py hello world
which:
    - calls ai_generate_code() (deterministic generator, no model weights)
    - writes the generated Python source into a free guest-physical address
    - launches `linux python mem:<addr>:<len>` -- the interpreter reads the
      code straight from guest memory (identity map) and executes it.
If the AI-generated source appears AND the guest exits cleanly, pass.

Usage: verify_ai_codegen.py [wait_seconds]
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
SERIAL = os.path.join(BUILD, "serial_ai_codegen.txt")
QERR = os.path.join(BUILD, "qemu_ai_codegen.err")
MON_PORT = 5597

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 90

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
print("[*] launching QEMU (pc/512M/tcg) for ai codegen test...")
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
type_keys(s, "ai py hello world")
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
    "[AI] generated Python source:",
    "[python] source: AI-generated code (passed via guest memory)",
    "[python] read_script('mem:",
    "Hello from the NexOS AI agent",
    "2 + 3 =",
    "Hello-world task complete",
):
    print(f"  {'FOUND ' if marker in serial else 'MISS  '} {marker}")

if "linux: process exited" in serial:
    print("  guest exited: YES (clean return)")
else:
    print("  guest exited: NO")

print("\n================ VERDICT ================")
gen_src   = "[AI] generated Python source:" in serial
ai_ran    = "[python] source: AI-generated code (passed via guest memory)" in serial
mem_read  = "[python] read_script('mem:" in serial
clean     = "linux: process exited" in serial
passed = gen_src and ai_ran and mem_read and clean
if passed:
    print("  PASS: OS AI engine authored Python at runtime, the interpreter")
    print("        loaded it via guest memory and RAN it; clean exit.")
else:
    print("  FAIL: see MISS markers above.")
    if not mem_read:
        print("        (python did NOT read from guest memory -- fell back to demo)")
print(f"\nserial: {SERIAL}")
sys.exit(0 if passed else 1)
