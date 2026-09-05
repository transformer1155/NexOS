import subprocess, socket, time, sys

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os.img'
MON  = ('127.0.0.1', 1235)

cmd = [
    QEMU, '-display', 'none',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(8)
try:
    s = socket.create_connection(MON, timeout=5)
    s.sendall(b'info pci\n')
    time.sleep(1)
    data = b''
    s.settimeout(2)
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk: break
            data += chunk
    except Exception:
        pass
    txt = data.decode('utf-8', 'replace')
    # show VGA-related lines
    for line in txt.splitlines():
        if 'VGA' in line or 'Display' in line or 'bar 0' in line.lower() or '03' in line:
            print(line)
    s.sendall(b'quit\n')
    s.close()
except Exception as e:
    print('err', repr(e))
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()
