#!/usr/bin/env python3
"""
test_iso.py - Test ISO BIOS boot by controlling QEMU via HMP socket.
Boots from CD in BIOS mode, with a hard drive attached for SFS access.
"""
import subprocess, socket, time, sys, os

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/os.iso"
HDD = sys.argv[2] if len(sys.argv) > 2 else "build/os.img"
SOCK = "/tmp/qemu_iso_hmp.sock"
SERIAL = "/tmp/iso_serial.txt"
VGA1 = "build/iso_vga1.bin"
VGA2 = "build/iso_vga2.bin"

os.makedirs("build", exist_ok=True)
for f in [VGA1, VGA2, SOCK, SERIAL]:
    try: os.remove(f)
    except: pass

# Start QEMU: boot from CD, with hard drive for SFS
qemu_cmd = [
    "qemu-system-x86_64",
    "-cdrom", ISO,
    "-boot", "d",
    "-drive", f"format=raw,file={HDD}",
    "-m", "64M",
    "-display", "none",
    "-no-reboot",
    "-no-shutdown",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"unix:{SOCK},server,nowait",
]
print(f"==> Starting QEMU: CD={ISO}, HDD={HDD}")
qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# Wait for the monitor socket
for _ in range(50):
    if os.path.exists(SOCK): break
    time.sleep(0.1)
else:
    print("ERROR: QEMU monitor socket not created")
    qemu.kill()
    sys.exit(1)

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(SOCK)
sock.settimeout(2.0)
try: sock.recv(4096)
except: pass

def send_cmd(cmd):
    sock.sendall((cmd + "\n").encode())
    time.sleep(0.1)
    try: return sock.recv(4096).decode(errors='replace')
    except socket.timeout: return ""

def sendkey(key, delay=0.03):
    send_cmd(f"sendkey {key}")
    time.sleep(delay)

def type_line(text, delay=0.15):
    for ch in text:
        if ch == ' ':    sendkey("spc")
        elif ch == '.':  sendkey("dot")
        elif ch == '/':  sendkey("slash")
        elif ch == '\\': sendkey("backslash")
        elif ch == '-':  sendkey("minus")
        elif ch == '_':  sendkey("shift-minus")
        elif ch.isupper(): sendkey(f"shift-{ch.lower()}")
        else:            sendkey(ch)
    sendkey("ret")
    time.sleep(delay)

# Wait for kernel to boot from CD
print("==> Waiting for kernel to boot from CD...")
time.sleep(6)

# Dump VGA (boot screen)
print("==> Dumping VGA (boot screen)...")
send_cmd(f"memsave 0xb8000 0x1000 {VGA1}")

# Type shell commands
print("==> Typing shell commands...")
type_line("help", 0.5)
type_line("echo hello_from_iso", 0.3)

# Dump VGA (after commands)
print("==> Dumping VGA (after commands)...")
send_cmd(f"memsave 0xb8000 0x1000 {VGA2}")

# Quit
send_cmd("quit")
time.sleep(0.5)
try: qemu.wait(timeout=5)
except: qemu.kill()

# Show serial output
print("\n===== Serial output =====")
try:
    with open(SERIAL, 'r') as f:
        serial = f.read()
    for line in serial.strip().split('\n'):
        print(f"  {line}")
except:
    print("(no serial)")

# Decode VGA dumps
def decode_vga(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return ""
    with open(path, 'rb') as f:
        d = f.read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    return c

def print_vga(path, label):
    print(f"\n===== {label} =====")
    c = decode_vga(path)
    if not c:
        print("(no dump)")
        return ""
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(line)
    return c

d1 = print_vga(VGA1, "VGA dump 1: boot screen")
d2 = print_vga(VGA2, "VGA dump 2: after shell commands")

# Assertions
print("\n===== Assertions =====")
ok = True
def chk(cond, msg):
    global ok
    result = "PASS" if cond else "FAIL"
    print(f"{result}: {msg}")
    if not cond: ok = False

chk('Hello world' in d1 or 'MiniOS' in d1, "kernel boots from ISO (banner visible)")
chk('DISK ERR' not in d1,                   "no disk read error")
chk('hello_from_iso' in d2,                 "shell executes echo command from ISO boot")
chk('shutdown' in d2 or 'Tab' in d2,        "help command works from ISO boot")

if ok:
    print("\n=== ISO BIOS BOOT TEST PASSED ===")
else:
    print("\n=== ISO BIOS BOOT TEST FAILED ===")

sys.exit(0 if ok else 1)
