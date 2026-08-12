#!/usr/bin/env python3
"""Verify kernel boots and VGA has content, using quoted memsave path."""
import subprocess, socket, time, os, sys, re

os.system('rm -f /tmp/NexOS_mon.sock /tmp/NexOS_serial.txt')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon.sock,server,nowait',
    '-serial', 'file:/tmp/NexOS_serial.txt',
]

p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(5)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
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

def send_and_wait(cmd, wait=1.0):
    s.sendall((cmd + '\n').encode())
    time.sleep(wait)
    resp = read_until_prompt(3)
    clean = re.sub(r'\x1b\[[^a-zA-Z]*[a-zA-Z]', '', resp.decode(errors='replace'))
    return clean

read_until_prompt(3)

# Dump VGA with QUOTED path
VGA = '/tmp/vga_boot.bin'
os.system('rm -f ' + VGA)
resp = send_and_wait(f'memsave 0xb8000 0x1000 "{VGA}"', 2.0)
print('memsave response:', resp[-200:])

if os.path.exists(VGA) and os.path.getsize(VGA) > 0:
    d = open(VGA, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    print('\n=== VGA boot screen ===')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(f'  {line}')
else:
    print('VGA file missing or empty!')

# Now try typing commands
def type_line(text, delay=0.15):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in text:
        if ch.isupper():
            send_and_wait(f'sendkey shift-{ch.lower()}', 0.03)
        else:
            key = keymap.get(ch, ch)
            send_and_wait(f'sendkey {key}', 0.03)
    send_and_wait('sendkey ret', delay)

# Login
print('\n=== Logging in as root ===')
type_line('root', 1.0)
type_line('admin', 2.0)

VGA2 = '/tmp/vga_login.bin'
os.system('rm -f ' + VGA2)
resp = send_and_wait(f'memsave 0xb8000 0x1000 "{VGA2}"', 2.0)
if os.path.exists(VGA2) and os.path.getsize(VGA2) > 0:
    d = open(VGA2, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    print('=== After login ===')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(f'  {line}')

# Run help
type_line('help', 1.0)
VGA3 = '/tmp/vga_help.bin'
os.system('rm -f ' + VGA3)
resp = send_and_wait(f'memsave 0xb8000 0x1000 "{VGA3}"', 2.0)
if os.path.exists(VGA3) and os.path.getsize(VGA3) > 0:
    d = open(VGA3, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    print('=== After help ===')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(f'  {line}')

# Run echo hello
type_line('echo hello', 0.5)
VGA4 = '/tmp/vga_echo.bin'
os.system('rm -f ' + VGA4)
resp = send_and_wait(f'memsave 0xb8000 0x1000 "{VGA4}"', 2.0)
if os.path.exists(VGA4) and os.path.getsize(VGA4) > 0:
    d = open(VGA4, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    print('=== After echo hello ===')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print(f'  {line}')

s.sendall(b'quit\n')
s.close()
p.wait(timeout=10)
