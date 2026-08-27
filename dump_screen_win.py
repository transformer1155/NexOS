import subprocess, socket, time, sys

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os.img'
OUT  = r'D:\MyOS\bootloader\shot.ppm'
MON  = ('127.0.0.1', 1236)
SER  = ('127.0.0.1', 1234)

cmd = [
    QEMU, '-display', 'none',
    '-serial', f'tcp:{SER[0]}:{SER[1]},server,nowait',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
print('launching qemu...', file=sys.stderr)
p = subprocess.Popen(cmd)
time.sleep(15)
try:
    s = socket.create_connection(MON, timeout=5)
    s.sendall(b'screendump ' + OUT.encode() + b'\n')
    time.sleep(2)
    s.sendall(b'quit\n')
    s.close()
    print('screendump issued ->', OUT, file=sys.stderr)
except Exception as e:
    print('monitor connect failed:', repr(e), file=sys.stderr)
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()
print('done', file=sys.stderr)
