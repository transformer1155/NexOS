#!/usr/bin/env python3
"""WSL test runner: boots NexOS in QEMU and captures VGA + serial output.
Usage (in WSL): python3 tools/_wsl_test.py <image> [commands...]
"""
import subprocess, socket, time, os, sys

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
MON_SOCK = "/tmp/NexOS_mon.sock"
SERIAL = "/tmp/NexOS_serial.txt"
VGA = "/tmp/NexOS_vga.bin"
SHOT = "/tmp/NexOS_shot.ppm"

os.system(f"rm -f {MON_SOCK} {SERIAL} {VGA} {SHOT}")

qemu = [
    "qemu-system-x86_64",
    "-drive", f"format=raw,file={IMG}",
    "-m", "64M", "-display", "none", "-no-reboot",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-serial", f"file:{SERIAL}",
]

# Add extra args (network, cdrom, etc.)
extra = sys.argv[2:]
if extra:
    qemu += extra

print(f"==> Starting QEMU: {IMG}")
p = subprocess.Popen(qemu, stdout=open("/dev/null", "w"), stderr=subprocess.STDOUT)

# Wait for boot
time.sleep(5)

# Connect to monitor
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connected = False
for i in range(20):
    try:
        s.connect(MON_SOCK)
        connected = True
        break
    except Exception:
        time.sleep(1)

if not connected:
    print("MONITOR CONNECT FAIL")
    p.kill()
    sys.exit(1)

s.settimeout(2.0)
try:
    s.recv(4096)
except:
    pass

def send_cmd(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.1)
    try:
        return s.recv(4096).decode(errors='replace')
    except socket.timeout:
        return ""

def sendkey(key, delay=0.03):
    send_cmd(f"sendkey {key}")
    time.sleep(delay)

def type_line(text, delay=0.15):
    keymap = {
        ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
        '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
        '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
        '(': 'shift-parenleft', ')': 'shift-parenright',
        ',': 'comma', '=': 'equal', '*': 'shift-8',
    }
    for ch in text:
        if ch.isupper():
            sendkey(f"shift-{ch.lower()}")
        else:
            key = keymap.get(ch, ch)
            sendkey(key)
    sendkey("ret")
    time.sleep(delay)

def dump_vga(tag=""):
    s.sendall(f"memsave 0xb8000 0x1000 {VGA}\n".encode())
    time.sleep(0.5)
    if os.path.exists(VGA):
        d = open(VGA, 'rb').read()
        c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
        lines = []
        for i in range(0, len(c), 80):
            line = c[i:i+80].rstrip('\x00')
            if line.strip():
                lines.append(line)
        if lines:
            print(f"--- VGA dump {tag} ---")
            for line in lines:
                print(f"  {line}")
        else:
            print(f"--- VGA dump {tag}: (blank) ---")
        return c
    print(f"--- VGA dump {tag}: (no file) ---")
    return ""

def screenshot(tag=""):
    s.sendall(f"screendump {SHOT}\n".encode())
    time.sleep(1)
    if os.path.exists(SHOT):
        sz = os.path.getsize(SHOT)
        print(f"--- Screenshot {tag}: {sz} bytes ---")
        # Copy to Windows-accessible path
        os.system(f"cp {SHOT} /mnt/d/MyOS/bootloader/build/shot_{tag}.ppm")
    else:
        print(f"--- Screenshot {tag}: (no file) ---")

# Wait for kernel to boot to login prompt
print("==> Waiting for kernel boot...")
time.sleep(3)

# Dump boot screen
vga = dump_vga("boot")

# Try logging in
print("==> Logging in as root...")
type_line("root", 1.0)
type_line("admin", 2.0)
vga = dump_vga("login")

# Run some commands
print("==> Running test commands...")
type_line("help", 1.0)
vga = dump_vga("help")

type_line("echo hello", 0.5)
vga = dump_vga("echo")

type_line("whoami", 0.5)
vga = dump_vga("whoami")

type_line("about", 0.5)
vga = dump_vga("about")

# Clean up
s.sendall(b"quit\n")
s.close()
try:
    p.wait(timeout=10)
except:
    p.kill()

# Print serial output
print("\n===== Serial Output =====")
if os.path.exists(SERIAL):
    ser = open(SERIAL, 'r', errors='replace').read()
    if ser.strip():
        for line in ser.strip().split('\n'):
            print(f"  {line}")
    else:
        print("  (empty)")
else:
    print("  (no serial file)")

print("\nDone.")
