import subprocess, socket, time, sys
QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os_v2.img'
OUT  = r'D:\MyOS\bootloader\shot_virtio.ppm'
MON  = ('127.0.0.1', 1237)
SER  = ('127.0.0.1', 1235)
# try virtio-gpu + egl-headless GL backend
cmd = [
    QEMU, '-vga', 'virtio', '-display', 'egl-headless,gl=on',
    '-serial', f'tcp:{SER[0]}:{SER[1]},server,nowait',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(18)
try:
    s = socket.create_connection(MON, timeout=5)
    s.sendall(b'screendump ' + OUT.encode() + b'\n')
    time.sleep(2)
    s.sendall(b'quit\n'); s.close()
except Exception as e:
    print('err', repr(e), file=sys.stderr)
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()
from PIL import Image
try:
    im = Image.open(OUT).convert('RGB'); w,h = im.size
    nonblack = sum(1 for y in range(0,h,4) for x in range(0,w,4) if im.getpixel((x,y)) != (0,0,0))
    print(f'virtio+gl: {w}x{h}, non-black sampled = {nonblack}/{ (w//4)*(h//4) }')
except Exception as e:
    print('no screenshot / black:', repr(e))
