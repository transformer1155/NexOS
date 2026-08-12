#!/usr/bin/env python3
"""WSL smoke test: boot NexOS in QEMU, capture VGA(0xB8000) + serial.
Run inside WSL: python3 tools/wsl_smoke.py [image]
Outputs decoded text to build/smoke_log.txt (also /tmp).
"""
import subprocess, socket, time, os, sys

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
MON_SOCK = "/tmp/NexOS_smoke_mon.sock"
SERIAL = "/tmp/NexOS_smoke_serial.txt"
VGA = "/tmp/NexOS_smoke_vga.bin"
LOG = "/mnt/d/MyOS/bootloader/build/smoke_log.txt"

os.system(f"rm -f {MON_SOCK} {SERIAL} {VGA}")

qemu = [
    "qemu-system-x86_64",
    "-drive", f"format=raw,file={IMG}",
    "-m", "64M", "-display", "none", "-no-reboot",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-serial", f"file:{SERIAL}",
    "-net", "none",
]
print(f"==> Starting QEMU: {IMG}")
p = subprocess.Popen(qemu, stdout=open("/dev/null", "w"), stderr=subprocess.STDOUT)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connected = False
for i in range(20):
    try:
        s.connect(MON_SOCK); connected = True; break
    except Exception:
        time.sleep(1)
if not connected:
    print("MONITOR CONNECT FAIL"); p.kill(); sys.exit(1)

s.settimeout(2.0)
try: s.recv(4096)
except: pass

def send_cmd(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.15)
    try: return s.recv(8192).decode(errors='replace')
    except socket.timeout: return ""

def sendkey(key, delay=0.04):
    send_cmd(f"sendkey {key}")
    time.sleep(delay)

KEYMAP = {
    ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
    '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
    '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
    '(': 'shift-parenleft', ')': 'shift-parenright',
    ',': 'comma', '=': 'equal', '*': 'shift-8', '\n': 'ret',
}

def type_line(text, delay=0.12):
    for ch in text:
        if ch.isupper():
            sendkey(f"shift-{ch.lower()}")
        else:
            sendkey(KEYMAP.get(ch, ch))
    sendkey("ret")
    time.sleep(delay)

def dump_vga(tag):
    s.sendall(f"memsave 0xb8000 0x1000 {VGA}\n".encode())
    time.sleep(0.4)
    out = ""
    if os.path.exists(VGA):
        d = open(VGA, 'rb').read()
        c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
        lines = []
        for i in range(0, len(c), 80):
            line = c[i:i+80].rstrip('\x00')
            if line.strip(): lines.append(line)
        out = "\n".join(lines)
    return out

log = []
def rec(tag, txt):
    log.append(f"\n===== {tag} =====")
    log.append(txt if txt else "(blank)")
    print(f"--- {tag} captured ({len(txt)} chars) ---")

# Boot wait
print("==> Waiting for boot...")
time.sleep(12)
rec("BOOT_SCREEN", dump_vga("boot"))

print("==> Login username root")
type_line("root", 1.0)
rec("AFTER_USER", dump_vga("user"))

print("==> Login password admin")
type_line("admin", 2.0)
rec("AFTER_PASS", dump_vga("pass"))

print("==> help")
type_line("help", 1.0)
rec("AFTER_HELP", dump_vga("help"))

print("==> echo hello")
type_line("echo smoke_test_ok", 0.8)
rec("AFTER_ECHO", dump_vga("echo"))

print("==> whoami")
type_line("whoami", 0.6)
rec("AFTER_WHOAMI", dump_vga("whoami"))

# Cleanup
s.sendall(b"quit\n"); s.close()
try: p.wait(timeout=10)
except: p.kill()

# Serial
ser = ""
if os.path.exists(SERIAL):
    ser = open(SERIAL, 'r', errors='replace').read()
log.append("\n===== SERIAL =====")
log.append(ser if ser.strip() else "(empty)")

with open(LOG, 'w') as f:
    f.write("\n".join(log))
os.system(f"cp {SERIAL} /mnt/d/MyOS/bootloader/build/smoke_serial.txt")
print(f"\nDone. Wrote {LOG}")
