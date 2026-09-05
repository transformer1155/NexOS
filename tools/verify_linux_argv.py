#!/usr/bin/env python3
"""Stage 4 verification: argv/envp transparency through the Linux-compat layer.

Exercises the freestanding ELF32 guest `linux_argv` end-to-end on the BIOS
os_textboot.img via QEMU sendkey:

  Run 1: `linux linux_argv hello world`
      -> proves argv from the `linux` command line reaches main()
         (argc=3, argv[1]=hello, argv[2]=world) and the *default* envp
         (envp0) is visible (PATH=/sbin:...).

  Run 2: `linux linux_argv spawn`
      -> the guest execve()s ITSELF with a custom envp
         (STAGE4=ENV_PASSED, FOO=BAR).  This proves execve's r->edx envp
         is really parsed by the kernel and delivered to the new image,
         instead of the hardcoded default set.

Both runs must be observed in the serial log with NO 'F' failures.

Usage: verify_linux_argv.py [wait_seconds]
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
SERIAL = os.path.join(BUILD, "serial_linux_argv.txt")
QERR = os.path.join(BUILD, "qemu_linux_argv.err")
MON_PORT = 5595

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
    QEMU, "-machine", "pc", "-m", "256", "-accel", "tcg,tb-size=128",
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
        # QEMU sendkey uses key NAMES, not literal chars.  Underscore is
        # Shift+Minus on a US layout; period and space have dedicated names.
        key = {" ": "spc", ".": "dot", "_": "shift-minus"}.get(ch, ch)
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

# now wait for the shell
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

# ---- Run 1: argv from the command line ----
drain()
print("[*] run 1: linux linux_argv hello world")
type_keys(s, "linux linux_argv hello world")
s.sendall(b"sendkey ret\n")
time.sleep(10)
drain()

# ---- Run 2: execve() with a custom envp ----
drain()
print("[*] run 2: linux linux_argv spawn")
type_keys(s, "linux linux_argv spawn")
s.sendall(b"sendkey ret\n")
time.sleep(14)
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

# Collect the LXARG evidence block(s).
lxarg_lines = [l.strip() for l in serial.splitlines() if "LXARG:" in l]

checks = {
    "argv passed (argc=3)":
        "LXARG: argc=3" in serial,
    "argv[1]=hello":
        "LXARG: argv[1]=hello" in serial,
    "argv[2]=world":
        "LXARG: argv[2]=world" in serial,
    "default envp visible (PATH=/sbin)":
        "LXARG: env[PATH=/sbin:/bin:/usr/sbin:/usr/bin]" in serial,
    "execve self triggered":
        "LXARG: execve self with custom envp" in serial,
    "execve envp reached (STAGE4=ENV_PASSED)":
        "LXARG: STAGE4=ENV_PASSED" in serial,
    "execve envp reached (FOO=BAR)":
        "LXARG: env[FOO=BAR]" in serial,
}
guest_exited = "process exited" in serial

print("\n================ EVIDENCE (LXARG lines) ================")
for ln in lxarg_lines[:60]:
    print("  " + ln[:120])
if len(lxarg_lines) > 60:
    print(f"  ... (+{len(lxarg_lines) - 60} more)")

print("\n================ CHECKS ================")
all_ok = True
for name, ok in checks.items():
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    all_ok = all_ok and ok
print(f"  [{'PASS' if guest_exited else 'FAIL'}] guest exited cleanly (process exited)")

print("\n================ VERDICT ================")
if all_ok and guest_exited:
    print("  GOOD: Stage 4 PASS -- argv from `linux` reaches main();")
    print("        execve() r->edx envp is parsed and delivered to the new image.")
else:
    print("  FAIL: one or more Stage 4 checks failed (see above).")
print(f"\nserial: {SERIAL}")
sys.exit(0 if (all_ok and guest_exited) else 1)
