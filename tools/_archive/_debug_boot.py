#!/usr/bin/env python3
"""Debug boot: check if QEMU starts, monitor responds, VGA has content.
Uses QMP-like approach: send command, wait for (qemu) prompt, read all."""
import subprocess, socket, time, os, sys

MON_SOCK = '/tmp/NexOS_mon.sock'
SERIAL = '/tmp/NexOS_serial.txt'
VGA = '/tmp/NexOS_vga.bin'

os.system('rm -f /tmp/NexOS_mon.sock /tmp/NexOS_serial.txt /tmp/NexOS_vga.bin')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon.sock,server,nowait',
    '-serial', 'file:/tmp/NexOS_serial.txt',
]

print('Starting QEMU:', ' '.join(qemu_cmd))
p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(5)

if p.poll() is not None:
    out, err = p.communicate()
    print('QEMU exited early! returncode:', p.returncode)
    print('STDOUT:', out.decode()[:1000])
    print('STDERR:', err.decode()[:1000])
    sys.exit(1)

print('QEMU is running, PID:', p.pid)

# Connect to monitor
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
try:
    s.connect(MON_SOCK)
    print('Connected to monitor')
except Exception as e:
    print('Connect failed:', e)
    p.kill()
    sys.exit(1)

def read_until_prompt(timeout=5):
    """Read from monitor until we see (qemu) prompt."""
    data = b''
    end_time = time.time() + timeout
    while time.time() < end_time:
        try:
            chunk = s.recv(4096)
            if chunk:
                data += chunk
                if b'(qemu) ' in data[-100:]:
                    break
        except socket.timeout:
            break
    return data

def send_and_wait(cmd, wait=2.0):
    """Send a command and wait for response."""
    s.sendall((cmd + '\n').encode())
    time.sleep(wait)
    return read_until_prompt(3)

# Read welcome
welcome = read_until_prompt(3)
print('Welcome:', repr(welcome[:200]))

# info status
resp = send_and_wait('info status', 1.0)
# Strip ANSI escape sequences for readability
import re
clean = re.sub(r'\x1b\[[^a-zA-Z]*[a-zA-Z]', '', resp.decode(errors='replace'))
print('info status:', clean.strip()[-200:])

# memsave - wait longer
resp = send_and_wait('memsave 0xb8000 0x1000 ' + VGA, 3.0)
clean = re.sub(r'\x1b\[[^a-zA-Z]*[a-zA-Z]', '', resp.decode(errors='replace'))
print('memsave response:', clean.strip()[-200:])

# Check if file was created
print('VGA file exists:', os.path.exists(VGA))
if os.path.exists(VGA) and os.path.getsize(VGA) > 0:
    print('VGA file size:', os.path.getsize(VGA))
    d = open(VGA, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    for i in range(0, len(c), 80):
        line = c[i:i+80].rstrip('\x00')
        if line.strip():
            print('VGA:', repr(line))
    if not any(c[i:i+80].strip() for i in range(0, len(c), 80)):
        print('VGA: all zeros/blank')
else:
    print('VGA file missing or empty')
    # Try pmemsave instead
    resp = send_and_wait('pmemsave 0xb8000 0x1000 ' + VGA, 3.0)
    clean = re.sub(r'\x1b\[[^a-zA-Z]*[a-zA-Z]', '', resp.decode(errors='replace'))
    print('pmemsave response:', clean.strip()[-200:])
    print('VGA file exists after pmemsave:', os.path.exists(VGA))
    if os.path.exists(VGA) and os.path.getsize(VGA) > 0:
        d = open(VGA, 'rb').read()
        c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
        for i in range(0, len(c), 80):
            line = c[i:i+80].rstrip('\x00')
            if line.strip():
                print('VGA:', repr(line))

# Try screendump
resp = send_and_wait('screendump /tmp/NexOS_shot.ppm', 2.0)
print('screendump done, exists:', os.path.exists('/tmp/NexOS_shot.ppm'))
if os.path.exists('/tmp/NexOS_shot.ppm'):
    print('screenshot size:', os.path.getsize('/tmp/NexOS_shot.ppm'))

s.sendall(b'quit\n')
s.close()
try:
    p.wait(timeout=10)
except:
    p.kill()

# Check serial
if os.path.exists(SERIAL):
    ser = open(SERIAL, 'rb').read()
    print('Serial size:', len(ser))
    if ser:
        print('Serial:', repr(ser[:500]))
    else:
        print('Serial: (empty)')
else:
    print('No serial file')
