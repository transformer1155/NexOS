#!/usr/bin/env python3
"""Debug boot with serial stdio and screendump"""
import subprocess, socket, time, os, re, signal

os.system('rm -f /tmp/NexOS_mon3.sock /tmp/NexOS_debug.ppm')

qemu_cmd = [
    'qemu-system-x86_64',
    '-drive', 'format=raw,file=build/os.img',
    '-m', '64M', '-display', 'none', '-no-reboot',
    '-monitor', 'unix:/tmp/NexOS_mon3.sock,server,nowait',
    '-serial', 'stdio',
    '-nographic',
]

p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

# Read all available serial output (with timeout)
start = time.time()
serial_data = b''
while time.time() - start < 15:
    # Check if process still running
    if p.poll() is not None:
        break
    # Use select-like polling
    try:
        import select
        r, _, _ = select.select([p.stdout], [], [], 0.5)
        if r:
            chunk = os.read(p.stdout.fileno(), 4096)
            if chunk:
                serial_data += chunk
                print(f"[t={time.time()-start:.1f}s] Got {len(chunk)} bytes", flush=True)
    except:
        time.sleep(0.5)
    # Also try non-blocking read
    try:
        chunk = p.stdout.read1(4096) if hasattr(p.stdout, 'read1') else None
        if chunk:
            serial_data += chunk
            print(f"[t={time.time()-start:.1f}s] read1: {len(chunk)} bytes", flush=True)
    except:
        pass

# Try to read remaining
try:
    remaining = p.stdout.read()
    if remaining:
        serial_data += remaining
except:
    pass

print("\n=== SERIAL DATA (raw) ===")
print(repr(serial_data[:3000]))

print("\n=== SERIAL DATA (ascii) ===")
try:
    print(serial_data.decode('ascii', errors='replace')[:3000])
except:
    pass

# Connect to monitor
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect('/tmp/NexOS_mon3.sock')
    
    def read_prompt(timeout=2):
        data = b''
        end = time.time() + timeout
        while time.time() < end:
            try:
                chunk = s.recv(4096)
                if chunk:
                    data += chunk
            except socket.timeout:
                break
        return data

    read_prompt(2)
    
    # Screendump (graphical mode)
    s.sendall(b'screendump /tmp/NexOS_debug.ppm\n')
    time.sleep(2)
    read_prompt(2)
    
    s.sendall(b'quit\n')
    s.close()
except Exception as e:
    print(f"Monitor error: {e}")

p.kill()
p.wait(timeout=5)

# Check screendump
if os.path.exists('/tmp/NexOS_debug.ppm'):
    sz = os.path.getsize('/tmp/NexOS_debug.ppm')
    print(f"\n=== Screendump exists: {sz} bytes ===")
else:
    print("\n=== No screendump ===")

print("\n=== QEMU return code ===")
print(f"returncode: {p.returncode}")
