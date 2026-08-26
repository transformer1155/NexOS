#!/usr/bin/env python3
"""Probe kernel-side agent/model commands for crashes (32-bit textboot).
Boots os_textboot.img, logs in via serial (root/admin), then types
`agent init`, `agent run hello`, `model env`, `model run qwen1.7b` and
watches the serial log for DIAG-FAULT / reboot / hang."""
import socket, time, subprocess, os, re

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
DISK  = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_agent.txt")
MON  = 5602

for f in [SERIAL]:
    if os.path.exists(f): os.remove(f)

cmd = [QEMU, "-machine", "pc", "-accel", "tcg",
       "-drive", f"format=raw,file={DISK}", "-m", "2048",
       "-vga", "std", "-display", "none",
       "-serial", f"file:{SERIAL}",
       "-monitor", f"tcp:127.0.0.1:{MON},server,nowait"]
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(30):
    time.sleep(1.0)
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5); break
    except (ConnectionRefusedError, OSError):
        if p.poll() is not None: break
if s is None:
    print("FATAL: monitor never came up"); sys.exit(1)
s.settimeout(1.2)

def cmdq(c, w=0.4):
    s.sendall((c + "\n").encode()); time.sleep(w)
    while True:
        try:
            d = s.recv(256)
            if b"(qemu)" in d: break
        except socket.timeout: break

def type_text(txt):
    for ch in txt:
        key = ch
        if ch == ' ': key = "spc"
        elif ch == '/': key = "slash"
        elif ch == '.': key = "dot"
        cmdq(f"sendkey {key}", 0.10)

def wait_log(needle, timeout=12):
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(0.5)
        try: txt = open(SERIAL, encoding="utf-8", errors="replace").read()
        except Exception: txt = ""
        if needle in txt: return txt
    return txt

def run_cmd(cmdline):
    type_text(cmdline)
    cmdq("sendkey ret", 0.6)
    time.sleep(0.8)
    try: return open(SERIAL, encoding="utf-8", errors="replace").read()
    except Exception: return ""

def log_tail():
    try:
        t = open(SERIAL, encoding="utf-8", errors="replace").read()
        return t[-500:]
    except Exception:
        return "(no log)"

def crashed():
    try: txt = open(SERIAL, encoding="utf-8", errors="replace").read()
    except Exception: return False, "(no log)"
    rb = len(re.findall(r"VBEFQ0SFBZ\[K0\] kmain entered", txt))
    f = len(re.findall(r"\[DIAG-FAULT\]", txt))
    return (rb > 1 or f > 0), f"reboot_markers={rb} faults={f}"

# wait for shell
wait_log("Shell ready", 45)
time.sleep(2)
print("== shell up ==")

# login: root / admin
run_cmd("root")
run_cmd("admin")
time.sleep(1)
print("== login attempted ==")

for c in ["agent init", "agent run hello", "model env", "model run qwen1.7b"]:
    print(f"--- cmd: {c}")
    run_cmd(c)
    time.sleep(1.5)
    bad, info = crashed()
    print(f"    after: {info}")
    if bad:
        print("    >>> CRASH DETECTED <<<")
        print(log_tail())
        break
    tail = log_tail()
    # print last meaningful line (strip ansi-ish noise)
    print("    tail:", tail.replace("\n", " | ")[-220:])

bad, info = crashed()
print("== final:", info)
s.sendall(b"quit\n")
try: p.wait(6)
except Exception: p.kill()
