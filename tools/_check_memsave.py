#!/usr/bin/env python3
"""Check QEMU 10.0 memsave syntax and try alternatives."""
import subprocess, socket, time, os, sys, re

os.system('rm -f /tmp/NexOS_mon.sock')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon.sock,server,nowait',
]

p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

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

# Read welcome
read_until_prompt(3)

# Check help for memsave
print("=== help memsave ===")
print(send_and_wait('help memsave', 1.0))

# Try with relative path (no /)
print("=== memsave with relative path ===")
os.system('rm -f vga_test.bin')
resp = send_and_wait('memsave 0xb8000 0x1000 vga_test.bin', 2.0)
print(resp[-300:])
print("File exists:", os.path.exists('vga_test.bin'))
if os.path.exists('vga_test.bin'):
    print("File size:", os.path.getsize('vga_test.bin'))

# Try with quoted path
print("\n=== memsave with quoted path ===")
os.system('rm -f /tmp/vga_quoted.bin')
resp = send_and_wait('memsave 0xb8000 0x1000 "/tmp/vga_quoted.bin"', 2.0)
print(resp[-300:])
print("File exists:", os.path.exists('/tmp/vga_quoted.bin'))

# Try QMP mode
print("\n=== Trying QMP ===")
s.sendall(b'quit\n')
s.close()
p.wait(timeout=10)

# Now try with QMP
os.system('rm -f /tmp/NexOS_qmp.sock')
qemu_qmp = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-qmp', 'unix:/tmp/NexOS_qmp.sock,server,nowait',
]

p2 = subprocess.Popen(qemu_qmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)

s2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s2.settimeout(5.0)
s2.connect('/tmp/NexOS_qmp.sock')

# Read QMP greeting
greeting = s2.recv(4096)
print("QMP greeting:", greeting[:200])

# Send qmp capabilities
s2.sendall(b'{"execute": "qmp_capabilities"}\n')
time.sleep(0.5)
resp = s2.recv(4096)
print("qmp_capabilities resp:", resp[:200])

# Query status
s2.sendall(b'{"execute": "query-status"}\n')
time.sleep(0.5)
resp = s2.recv(4096)
print("query-status resp:", resp[:200])

# Try memsave via QMP
import json
cmd = json.dumps({
    "execute": "memsave",
    "arguments": {
        "val": "0xb8000",
        "size": "0x1000",
        "filename": "/tmp/vga_qmp.bin"
    }
})
s2.sendall((cmd + '\n').encode())
time.sleep(1)
resp = s2.recv(4096)
print("memsave resp:", resp[:200])
print("QMP VGA file exists:", os.path.exists('/tmp/vga_qmp.bin'))
if os.path.exists('/tmp/vga_qmp.bin'):
    print("QMP VGA file size:", os.path.getsize('/tmp/vga_qmp.bin'))
    d = open('/tmp/vga_qmp.bin', 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print('VGA:', repr(line))

# Quit
s2.sendall(b'{"execute": "quit"}\n')
s2.close()
p2.wait(timeout=10)
