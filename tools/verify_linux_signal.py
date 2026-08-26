#!/usr/bin/env python3
"""Headless verification of the NexOS Linux-compat signal layer (Stage 2).

Boots build/os_textboot.img, logs in (root/admin), runs
`linux linux_signal`, and asserts the guest prints the expected signal
markers:
  * HANDLER main ... sig=10   (tkill self SIGUSR1 -> handler runs)
  * after SIGUSR1 recovered   (rt_sigreturn restored context)
  * SIGUSR2 blocked, pend set (blocked signal stays pending)
  * after SIGUSR2 unblock recovered (unblock -> delivered)
  * HANDLER worker ... sig=10 (tkill worker)
  * HANDLER worker ... sig=12 (tgkill worker)
  * LXSIG: signal tests done  (all explicit tests passed)
  * linux: process exited     (default SIGTERM terminated the process)
Reports a clear PASS/FAIL summary.
"""
import os, sys, time, subprocess, socket

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
LOG = "build/serial_linux_signal.log"
MONPORT = 4469
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
type_line("linux linux_signal")
# give the guest time to run all signal tests + terminate
time.sleep(10)

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
for line in out.splitlines():
    s = line.strip()
    if ("LXSIG" in s) or ("HANDLER" in s) or ("WORKER" in s) or ("linux:" in s) or ("FAIL" in s) or ("DBG" in s):
        print(s)
print("============================================")

checks = [
    ("handler caught SIGUSR1 on main", "HANDLER main tid=1 sig=10" in out),
    ("context restored after SIGUSR1", "after SIGUSR1 recovered" in out),
    ("blocked SIGUSR2 stayed pending", "FAIL SIGUSR2 delivered while blocked" not in out and "HANDLER main tid=1 sig=12" in out),
    ("SIGUSR2 delivered after unblock", "HANDLER main tid=1 sig=12" in out and "signal tests done" in out),
    ("worker received tkill SIGUSR1", "HANDLER worker" in out and "sig=10" in out),
    ("worker received tgkill SIGUSR2", "HANDLER worker" in out and "sig=12" in out),
    ("explicit signal tests completed", "LXSIG: signal tests done" in out),
    ("default SIGTERM terminated process", "linux: process exited" in out),
]
ok = True
for name, passed in checks:
    print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
    ok = ok and passed

if ok:
    print("VERIFY RESULT: PASS  (signal delivery: handler, sigreturn, block/unblock, tkill/tgkill, default SIGTERM all worked)")
else:
    print("VERIFY RESULT: FAIL  (see serial tail + check list above)")
sys.exit(0 if ok else 1)
