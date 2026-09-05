#!/usr/bin/env python3
"""Headless verification of the NexOS Linux-compat mmap layer (Stage 3).

Boots build/os_textboot.img, logs in (root/admin), runs
`linux linux_mmap`, and asserts the guest prints the expected mmap markers:
  * LXMMAP: PASS mmap PROT_EXEC returned VA
  * LXMMAP: executed -> 0x00001234        (machine code in mmap'd page ran)
  * LXMMAP: PASS executable page ran (returned 0x1234)
  * LXMMAP: PASS mprotect RO then RW returned 0
  * LXMMAP: PASS data survived mprotect round-trip
  * LXMMAP: PASS munmap returned 0
  * LXMMAP: PASS remap after munmap usable
  * LXMMAP: PASS 4 MiB arena mmap fully writable
  * LXMMAP: all tests done
  * linux: process exited
Reports a clear PASS/FAIL summary.
"""
import os, sys, time, subprocess, socket

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
LOG = "build/serial_linux_mmap.log"
MONPORT = 4471
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

MON = f"tcp:127.0.0.1:{MONPORT}"
cmd = [
    QEMU, "-machine", "pc", "-m", "256M", "-accel", "tcg,tb-size=128", "-vga", "std",
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
type_line("linux linux_mmap")
# give the guest time to run all mmap tests + terminate
time.sleep(8)

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
for line in out.splitlines():
    s = line.strip()
    if ("LXMMAP" in s) or ("linux:" in s) or ("FAIL" in s) or ("EXCEPTION" in s) or ("HALT" in s):
        print(s)
print("============================================")

checks = [
    ("mmap PROT_EXEC returned VA", "LXMMAP: PASS mmap PROT_EXEC returned VA" in out),
    ("executable page actually ran (returned 0x1234)", "LXMMAP: PASS executable page ran (returned 0x1234)" in out),
    ("mprotect RO->RW returned 0", "LXMMAP: PASS mprotect RO then RW returned 0" in out),
    ("data survived mprotect round-trip", "LXMMAP: PASS data survived mprotect round-trip" in out),
    ("munmap returned 0", "LXMMAP: PASS munmap returned 0" in out),
    ("remap after munmap usable", "LXMMAP: PASS remap after munmap usable" in out),
    ("4 MiB arena mmap fully writable", "LXMMAP: PASS 4 MiB arena mmap fully writable" in out),
    ("all explicit tests completed", "LXMMAP: all tests done" in out),
    ("no FAIL marker", "LXMMAP: FAIL" not in out),
    ("default exit terminated process", "linux: process exited" in out),
]
ok = True
for name, passed in checks:
    print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
    ok = ok and passed

if ok:
    print("VERIFY RESULT: PASS  (Stage 3: mmap PROT_EXEC executable, mprotect RW-toggle, munmap, expanded 4MiB arena all worked)")
else:
    print("VERIFY RESULT: FAIL  (see serial tail + check list above)")
sys.exit(0 if ok else 1)
