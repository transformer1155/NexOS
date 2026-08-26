#!/usr/bin/env python3
"""Headless verification of the NexOS Linux-compat threading layer (Stage 1).

Boots build/os_textboot.img, logs in (root/admin), runs
`linux linux_threads`, and asserts the guest prints the PASS marker plus
the expected final counter.  Reports a clear PASS/FAIL summary.
"""
import os, sys, time, subprocess, socket

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
LOG = "build/serial_linux_threads.log"
MONPORT = 4467
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

MON = f"tcp:127.0.0.1:{MONPORT}"
cmd = [
    QEMU, "-machine", "pc", "-m", "256M", "-accel", "tcg", "-vga", "std",
    "-display", "none", "-no-reboot",
    "-monitor", f"tcp:127.0.0.1:{MONPORT},server,nowait",
    "-chardev", f"file,id=ser,path={LOG}", "-serial", "chardev:ser",
    "-drive", f"file={IMG},format=raw,if=ide,index=0,media=disk",
]
print("launching", " ".join(cmd))
if os.path.exists(LOG):
    os.remove(LOG)
p = subprocess.Popen(cmd)

def mon_connect():
    end = time.time() + 20
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", MONPORT), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

mon = mon_connect()

def key(k):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(0.06)

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
    key("ret"); time.sleep(0.35)

time.sleep(3)
type_line("root")
type_line("admin")
time.sleep(1.2)
type_line("linux linux_threads")
# give the guest time to spawn threads, count, join, exit
time.sleep(7)

# read what the guest printed
out = ""
if os.path.exists(LOG):
    with open(LOG, "r", errors="replace") as f:
        out = f.read()

mon.sendall(b"quit\n")
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()

print("================ SERIAL TAIL ================")
# show only lines mentioning our markers + any linux: diag
for line in out.splitlines():
    s = line.strip()
    if ("LXTHREADS" in s) or ("THREAD" in s) or ("linux:" in s) or ("PASS" in s) or ("FAIL" in s):
        print(s)
print("============================================")

ok = ("LXTHREADS: PASS" in out)
# expected counter = N_THREADS(4) * PER_THREAD(50000) = 200000
ok = ok and ("counter=200000" in out)
if ok:
    print("VERIFY RESULT: PASS  (multi-thread clone + shared counter + join all worked)")
else:
    print("VERIFY RESULT: FAIL  (see serial tail above)")
sys.exit(0 if ok else 1)
