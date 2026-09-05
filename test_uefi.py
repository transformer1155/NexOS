#!/usr/bin/env python3
"""
test_uefi.py - Test UEFI boot (ISO or Hybrid USB) via QEMU with OVMF.
"""
import subprocess, socket, time, sys, os

# Args: mode (iso|hybrid), image_path
MODE = sys.argv[1] if len(sys.argv) > 1 else "iso"
IMG  = sys.argv[2] if len(sys.argv) > 2 else ("build/os.iso" if MODE == "iso" else "build/os_hybrid.img")
HDD  = sys.argv[3] if len(sys.argv) > 3 else "build/os.img"

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE.fd"
OVMF_VARS_SRC = "/usr/share/OVMF/OVMF_VARS.fd"
OVMF_VARS = "build/ovmf_vars.fd"
SOCK = "/tmp/qemu_uefi.sock"
SERIAL = "/tmp/uefi_serial.txt"
VGA1 = "build/uefi_vga1.bin"
VGA2 = "build/uefi_vga2.bin"

os.makedirs("build", exist_ok=True)
for f in [VGA1, VGA2, SOCK, SERIAL]:
    try: os.remove(f)
    except: pass

# Copy OVMF vars (writable copy)
import shutil
shutil.copy2(OVMF_VARS_SRC, OVMF_VARS)

# Build QEMU command based on mode
qemu_cmd = [
    "qemu-system-x86_64",
    "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
    "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
    "-drive", f"format=raw,file={HDD}",   # hard drive for SFS
    "-m", "128M",
    "-display", "none",
    "-no-reboot",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"unix:{SOCK},server,nowait",
]
if MODE == "iso":
    qemu_cmd.insert(4, "-cdrom")
    qemu_cmd.insert(5, IMG)
    print(f"==> UEFI ISO boot test: {IMG}")
else:
    qemu_cmd.insert(4, "-drive")
    qemu_cmd.insert(5, f"format=raw,file={IMG}")
    print(f"==> UEFI Hybrid USB boot test: {IMG}")

qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# Wait for monitor socket
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

# UEFI boot takes longer
print("==> Waiting for UEFI boot (8s)...")
time.sleep(8)

# Dump VGA (boot screen)
print("==> Dumping VGA (boot screen)...")
send_cmd(f"memsave 0xb8000 0x1000 {VGA1}")

# Type shell commands
print("==> Typing shell commands...")
type_line("echo hello_from_uefi", 0.3)

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
    for line in serial.strip().split('\n')[:20]:
        print(f"  {line}")
except:
    print("(no serial)")

# Decode VGA
def decode_vga(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return ""
    with open(path, 'rb') as f:
        d = f.read()
    return bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')

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

chk('Hello world' in d1 or 'kernel' in d1.lower(), f"kernel boots via UEFI ({MODE})")
chk('hello_from_uefi' in d2,                        "shell executes echo command via UEFI")

if ok:
    print(f"\n=== UEFI {MODE.upper()} BOOT TEST PASSED ===")
else:
    print(f"\n=== UEFI {MODE.upper()} BOOT TEST FAILED ===")

sys.exit(0 if ok else 1)
