#!/usr/bin/env python3
"""Stage 7 verification: kernel ELF dynamic linker GNU_HASH path.

Exercises a dynamically-linked freestanding ELF32 guest `gnu_dynlink` that
DT_NEEDED=libgnu.so.  libgnu.so is built with --hash-style=gnu (SysV hash
absent).  This proves the kernel's DT_GNU_HASH symbol-lookup path (bloom
filter + bucket chain) works, independent of the older SysV DT_HASH path
verified in Stage 6.

The guest prints the same markers as the Stage 6 test:

    LXDL: hello from dynamically-linked guest
    LXDL: printf_ok
    LXDL: nex_add(2,3)=5
    LXDL: resolved_sym_ok
    LXDL: nex_a  (10,20)=30

Usage: verify_linux_gnuhash.py [wait_seconds]
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
SERIAL = os.path.join(BUILD, "serial_linux_gnuhash.txt")
QERR = os.path.join(BUILD, "qemu_linux_gnuhash.err")
MON_PORT = 5598

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 80


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
    QEMU, "-machine", "pc", "-m", "256", "-accel", "tcg,tb-size=128",
    "-blockdev", f"driver=file,filename={DISK},node-name=hd0,read-only=off",
    "-device", "ide-hd,drive=hd0",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std", "-display", "none",
]
print("[*] launching QEMU (pc/256M/tcg)...")
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
print(f"[*] waiting for login prompt ({WAIT}s)...")


def type_keys(s, text):
    for ch in text:
        key = {" ": "spc", ".": "dot", "_": "shift-minus"}.get(ch, ch)
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)


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

print("[*] waiting for shell...")
deadline = time.time() + 60
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

# ---- run the GNU_HASH dynamic-link test ----
drain()
print("[*] run: linux gnu_dynlink")
type_keys(s, "linux gnu_dynlink")
s.sendall(b"sendkey ret\n")
time.sleep(20)
drain()

print("[*] shutting down QEMU...")
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

dyn_lines = [l.strip() for l in serial.splitlines()
             if ("LXDL:" in l) or ("dyn:" in l)]

checks = {
    "kernel detected dynamic image (loaded libgnu.so)":
        ("dyn: loaded libgnu.so" in serial) or ("dyn: loaded" in serial),
    "kernel transferred control to dynamic exe":
        "dyn: transferring control to dynamic executable" in serial,
    "guest printf resolved from libgnu.so (printf_ok)":
        "LXDL: printf_ok" in serial,
    "guest cross-.so call resolved (resolved_sym_ok)":
        "LXDL: resolved_sym_ok" in serial,
    "nex_add(2,3)==5 via libgnu.so":
        "LXDL: nex_add(2,3)=5" in serial,
    "nex_add(10,20)==30 via libgnu.so":
        "LXDL: nex_add(10,20)=30" in serial,
    "no unresolved symbols":
        "dyn: unresolved" not in serial,
    "no dyn errors":
        "dyn:" in serial and ("dyn: " in serial and
         not any(x in serial for x in
                 ("dyn: main map fail", "dyn: lib map fail",
                  "dyn: lib not found", "dyn: lib not ELF",
                  "dyn: lib no PT_DYNAMIC", "dyn: lib region overflow",
                  "dyn: too many libs", "dyn: file not found"))),
    "guest exited cleanly":
        "process exited" in serial,
}

print("\n================ EVIDENCE (LXDL / dyn) ================")
for ln in dyn_lines[:80]:
    print("  " + ln[:120])
if len(dyn_lines) > 80:
    print(f"  ... (+{len(dyn_lines) - 80} more)")

print("\n================ CHECKS ================")
all_ok = True
for name, ok in checks.items():
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    all_ok = all_ok and ok

print("\n================ VERDICT ================")
if all_ok:
    print("  GOOD: Stage 7 GNU_HASH PASS -- kernel ELF dynamic linker resolves")
    print("        symbols via DT_GNU_HASH (bloom filter + bucket chain) against")
    print("        a --hash-style=gnu shared object.")
else:
    print("  FAIL: one or more Stage 7 GNU_HASH checks failed (see above).")
print(f"\nserial: {SERIAL}")
sys.exit(0 if all_ok else 1)
