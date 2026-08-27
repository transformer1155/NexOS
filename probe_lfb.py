import subprocess, socket, time, sys

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os.img'
MON  = ('127.0.0.1', 1245)

cmd = [
    QEMU, '-display', 'none',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(18)  # let GUI boot + render
try:
    s = socket.create_connection(MON, timeout=5)
    # full info pci
    s.sendall(b'info pci\n')
    time.sleep(1)
    s.settimeout(2)
    data=b''
    try:
        while True:
            c=s.recv(4096)
            if not c: break
            data+=c
    except Exception: pass
    for line in data.decode('utf-8','replace').splitlines():
        if 'VGA' in line or '1234' in line or 'bar' in line.lower() or 'Region' in line:
            print('PCI>', line)
    # dump what kernel thinks is LFB (0xFD000000) 3MB
    s.sendall(b'pmemsave 0xFD000000 0x300000 D:\\MyOS\\bootloader\\fdump.bin\n')
    time.sleep(1)
    print('pmemsave issued')
    s.sendall(b'quit\n')
    s.close()
except Exception as e:
    print('err', repr(e))
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()
