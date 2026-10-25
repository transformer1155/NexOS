#!/usr/bin/env python3
"""Stage 5 verification: Linux socket bridge (guest <-> host TCP echo).

Exercises the freestanding ELF32 guest `linux_net` end-to-end on the BIOS
os_textboot.img via QEMU sendkey + a REAL TCP echo server running on the
Windows host (QEMU user-net presents the host as 10.0.2.2):

    guest `linux linux_net`  ->  connect 10.0.2.2:18099
                              ->  send   "HELLO_FROM_GUEST"
                              ->  recv   echo back
                              ->  prints "LXNET: ECHO_OK [HELLO_FROM_GUEST]"

A host echo server is started on 0.0.0.0:18099 for the duration of the run.

Usage: verify_linux_net.py [wait_seconds]
"""
import os
import socket
import subprocess
import sys
import threading
import time

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_linux_net.txt")
QERR = os.path.join(BUILD, "qemu_linux_net.err")
MON_PORT = 5596
ECHO_PORT = 18099

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 70

# ----------------------------------------------------------------------
# Host TCP echo server (serves the guest's connect)
# ----------------------------------------------------------------------
echo_errors = []


def echo_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", ECHO_PORT))
    except OSError as e:
        echo_errors.append(f"bind 0.0.0.0:{ECHO_PORT} failed: {e}")
        return
    srv.listen(5)
    srv.settimeout(1.0)
    while not stop_echo.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            conn.settimeout(5.0)
            data = conn.recv(4096)
            if data:
                conn.sendall(data)   # echo
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
    try:
        srv.close()
    except OSError:
        pass


stop_echo = threading.Event()
echo_thread = threading.Thread(target=echo_server, daemon=True)
echo_thread.start()
time.sleep(0.5)
if echo_errors:
    print("[FATAL]", echo_errors[0])
    sys.exit(3)


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
    "-net", "nic,model=ne2k_isa", "-net", "user",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std", "-display", "none",
]
print("[*] launching QEMU (pc/256M/tcg + ne2k_isa user-net)...")
errf = open(QERR, "wb")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf)

s = None
for _ in range(20):
    if p.poll() is not None:
        errf.close()
        print(f"[FATAL] QEMU exited early rc={p.returncode}")
        with open(QERR, "r", errors="replace") as fh:
            print("  stderr:", fh.read().strip()[:400] or "(empty)")
        stop_echo.set()
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


# textboot drops to the security login prompt -> login as root/admin
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

# wait for the shell
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
    stop_echo.set()
    print("[FAIL] shell never became ready")
    sys.exit(1)


# net up (belt-and-suspenders; the guest syscall lazily inits the NIC too)
drain()
print("[*] net up")
type_keys(s, "net up")
s.sendall(b"sendkey ret\n")
time.sleep(4)
drain()

# ---- run the socket bridge test ----
drain()
print("[*] run: linux linux_net")
type_keys(s, "linux linux_net")
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
stop_echo.set()

# ---- analyse ----
serial = ""
if os.path.exists(SERIAL):
    with open(SERIAL, "r", errors="replace") as fh:
        serial = fh.read()

lxnet_lines = [l.strip() for l in serial.splitlines() if "LXNET:" in l]

checks = {
    "guest socket() ok (fd=0)":
        "LXNET: socket fd=0" in serial,
    "guest connect() ok (rc=0)":
        "LXNET: connect rc=0" in serial,
    "guest send ok (16 bytes)":
        "LXNET: sent 16 bytes" in serial,
    "guest recv ok (16 bytes)":
        "LXNET: recv 16 bytes" in serial,
    "echo round-trip matches (ECHO_OK)":
        "LXNET: ECHO_OK [HELLO_FROM_GUEST]" in serial,
    "no send failure":
        "LXNET: send FAILED" not in serial,
    "no recv mismatch":
        "LXNET: recv MISMATCH" not in serial,
    "guest exited cleanly":
        "process exited" in serial,
}

print("\n================ EVIDENCE (LXNET lines) ================")
for ln in lxnet_lines[:60]:
    print("  " + ln[:120])
if len(lxnet_lines) > 60:
    print(f"  ... (+{len(lxnet_lines) - 60} more)")

print("\n================ CHECKS ================")
all_ok = True
for name, ok in checks.items():
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    all_ok = all_ok and ok

print("\n================ VERDICT ================")
if all_ok:
    print("  GOOD: Stage 5 PASS -- guest TCP connect/send/recv reaches a")
    print("        real host echo server through the kernel NE2000 stack;")
    print("        the round-trip echo matches.")
else:
    print("  FAIL: one or more Stage 5 checks failed (see above).")
print(f"\nserial: {SERIAL}")
sys.exit(0 if all_ok else 1)
