#!/usr/bin/env python3
"""Simple boot test: serial to file + screendump"""
import subprocess, socket, time, os

os.system('rm -f /tmp/NexOS_mon4.sock /tmp/NexOS_serial4.txt /tmp/NexOS_screen4.ppm')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon4.sock,server,nowait',
    '-serial', 'file:/tmp/NexOS_serial4.txt',
]

p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(10)

# Check serial file
sz = 0
if os.path.exists('/tmp/NexOS_serial4.txt'):
    sz = os.path.getsize('/tmp/NexOS_serial4.txt')
    with open('/tmp/NexOS_serial4.txt', 'rb') as f:
        data = f.read()
    print(f"Serial file: {sz} bytes")
    if data:
        print("Serial raw:", repr(data[:500]))
        try:
            print("Serial text:", data.decode('ascii', errors='replace')[:500])
        except:
            pass
else:
    print("No serial file!")

# Connect to monitor
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect('/tmp/NexOS_mon4.sock')
    time.sleep(1)
    
    # Read welcome banner
    data = b''
    end = time.time() + 2
    while time.time() < end:
        try:
            chunk = s.recv(4096)
            if chunk: data += chunk
        except socket.timeout: break
    
    # Send commands
    for cmd in ['info status', 'info registers', 'screendump /tmp/NexOS_screen4.ppm']:
        s.sendall((cmd + '\n').encode())
        time.sleep(1.5)
        try:
            chunk = s.recv(4096)
            data += chunk
        except: pass
    
    clean = data.decode(errors='replace')
    print("\n=== Monitor output (last 2000 chars) ===")
    print(clean[-2000:])
    
    s.sendall(b'quit\n')
    s.close()
except Exception as e:
    print(f"Monitor error: {e}")

p.wait(timeout=5)

# Check screendump
if os.path.exists('/tmp/NexOS_screen4.ppm'):
    print(f"\nScreendump: {os.path.getsize('/tmp/NexOS_screen4.ppm')} bytes")
else:
    print("\nNo screendump!")
