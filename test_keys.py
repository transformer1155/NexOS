import subprocess, socket, time, sys

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os_v2.img'
OUT  = r'D:\MyOS\bootloader\shot.ppm'
OUT2 = r'D:\MyOS\bootloader\shot_keys.ppm'
MON  = ('127.0.0.1', 1236)
SER  = ('127.0.0.1', 1234)

cmd = [
    QEMU, '-display', 'none', '-vga', 'std',
    '-serial', f'tcp:{SER[0]}:{SER[1]},server,nowait',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(15)
try:
    s = socket.create_connection(MON, timeout=5)
    s.sendall(b'screendump ' + OUT.encode() + b'\n')
    time.sleep(1)
    # type a username: Tab to username field, then type "ADMIN"
    s.sendall(b'sendkey tab\n')
    time.sleep(0.3)
    for ch in 'ADMIN':
        s.sendall(f'sendkey {ch}\n'.encode())
        time.sleep(0.3)
    time.sleep(1)
    s.sendall(b'screendump ' + OUT2.encode() + b'\n')
    time.sleep(1)
    s.sendall(b'quit\n')
    s.close()
    print('done', file=sys.stderr)
except Exception as e:
    print('err:', repr(e), file=sys.stderr)
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()

# compare the two screenshots in the login-card region
from PIL import Image
a = Image.open(OUT).convert('RGB'); b = Image.open(OUT2).convert('RGB')
w,h = a.size
diff = 0
for y in range(h):
    for x in range(w):
        if a.getpixel((x,y)) != b.getpixel((x,y)):
            diff += 1
print(f'pixel diff between before/after typing: {diff}')
