#!/usr/bin/env python3
"""Test different QEMU disk interface options to find the BIOS boot issue."""
import subprocess, socket, time, os, sys, re

def test_boot(qemu_args, label):
    """Boot with given QEMU args and check VGA."""
    sock_path = '/tmp/test_mon.sock'
    ser_path = '/tmp/test_ser.txt'
    vga_path = '/tmp/test_vga.bin'
    os.system(f'rm -f {sock_path} {ser_path} {vga_path}')

    qemu_cmd = ['qemu-system-x86_64'] + qemu_args + [
        '-m', '64M', '-display', 'none', '-no-reboot',
        '-monitor', f'unix:{sock_path},server,nowait',
        '-serial', f'file:{ser_path}',
    ]

    print(f'\n=== {label} ===')
    print(f'QEMU: {" ".join(qemu_cmd)}')

    p = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(5)

    if p.poll() is not None:
        out, err = p.communicate()
        print(f'QEMU exited early! rc={p.returncode}')
        print(f'STDERR: {err.decode(errors="replace")[:500]}')
        return None

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect(sock_path)

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

        # Dump VGA with quoted path
        s.sendall(f'memsave 0xb8000 0x1000 "{vga_path}"\n'.encode())
        time.sleep(2)
        read_until_prompt(3)

        s.sendall(b'quit\n')
        s.close()
    except Exception as e:
        print(f'Monitor error: {e}')

    try:
        p.wait(timeout=10)
    except:
        p.kill()

    # Check serial
    ser = ''
    if os.path.exists(ser_path):
        ser = open(ser_path, 'rb').read()
        ser_text = ser.decode('latin-1', errors='replace')
        print(f'Serial ({len(ser)} bytes): {repr(ser_text[:200])}')

    # Check VGA
    if os.path.exists(vga_path) and os.path.getsize(vga_path) > 0:
        d = open(vga_path, 'rb').read()
        c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
        lines = []
        for i in range(0, len(c), 80):
            line = c[i:i+80].rstrip('\x00')
            if line.strip():
                lines.append(line)
        if lines:
            print('VGA:')
            for line in lines:
                print(f'  {line}')
        else:
            print('VGA: (blank)')
        return lines
    else:
        print('VGA: (no file)')
        return None

# Test 1: Default -drive format=raw
test_boot(['-drive', 'format=raw,file=build/os.img'], 'Default -drive format=raw')

# Test 2: With if=ide
test_boot(['-drive', 'file=build/os.img,format=raw,if=ide'], 'if=ide')

# Test 3: -hda
test_boot(['-hda', 'build/os.img'], '-hda')

# Test 4: With machine=pc
test_boot(['-machine', 'pc', '-drive', 'format=raw,file=build/os.img'], 'machine=pc')

# Test 5: With if=floppy (fda)
test_boot(['-fda', 'build/os.img'], '-fda (floppy)')
