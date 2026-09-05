#!/usr/bin/env python3
"""Debug: check serial output and VGA dump from QEMU"""
import subprocess, socket, time, os, re

os.system('rm -f /tmp/NexOS_mon.sock /tmp/NexOS_serial.txt')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon.sock,server,nowait',
    '-serial', 'file:/tmp/NexOS_serial.txt',
]

p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(8)

# Read serial output
if os.path.exists('/tmp/NexOS_serial.txt'):
    with open('/tmp/NexOS_serial.txt', 'rb') as f:
        serial = f.read()
    print("=== SERIAL OUTPUT (full) ===")
    print(repr(serial[:2000]))
    # Try ASCII
    try:
        print("\n=== SERIAL ASCII ===")
        print(serial.decode('ascii', errors='replace')[:2000])
    except:
        pass
else:
    print("No serial file!")

# Connect to monitor and dump VGA
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5.0)
s.connect('/tmp/NexOS_mon.sock')

def read_until_prompt(timeout=3):
    data = b''
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(4096)
            if chunk:
                data += chunk
                if b'(qemu) ' in data[-100:]:
                    break
        except socket.timeout:
            break
    return data

read_until_prompt(3)

# Dump VGA with PNG screenshot
s.sendall(b'screendump /tmp/vga_debug.ppm\n')
time.sleep(2)
read_until_prompt(2)

# Also dump text mode VGA
s.sendall(b'memsave 0xb8000 0x1000 "/tmp/vga_debug.bin"\n')
time.sleep(2)
read_until_prompt(2)

s.sendall(b'quit\n')
s.close()
p.wait(timeout=10)

# Analyze VGA text
if os.path.exists('/tmp/vga_debug.bin') and os.path.getsize('/tmp/vga_debug.bin') > 0:
    d = open('/tmp/vga_debug.bin', 'rb').read()
    # Check for any non-zero content
    non_zero = [(i, d[i]) for i in range(min(200, len(d))) if d[i] != 0]
    print(f"\n=== VGA first 200 bytes, {len(non_zero)} non-zero ===")
    for idx, val in non_zero[:40]:
        print(f"  offset {idx}: 0x{val:02x} ({chr(val) if 32 <= val < 127 else '?'})")

    # Try character extraction
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    print('\n=== VGA text ===')
    for i in range(0, min(len(c), 2000), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(f'  [{i//80}] {repr(line)}')
        else:
            empty_count = sum(1 for j in range(i, min(i+80, len(c), 2000)) if c[j] == '\x00')
            if empty_count < 80:
                print(f'  [{i//80}] (some non-zero chars)')
else:
    print("No VGA file!")
