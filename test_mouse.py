import subprocess, socket, time, sys

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os_v2.img'
OUT  = r'D:\MyOS\bootloader\shot.ppm'
MON  = ('127.0.0.1', 1236)
SER  = ('127.0.0.1', 1234)

cmd = [
    QEMU, '-vnc', ':1', '-vga', 'std',
    '-serial', f'tcp:{SER[0]}:{SER[1]},server,nowait',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(15)
try:
    s = socket.create_connection(MON, timeout=5)
    # simulate mouse moving to several positions (like a user sweeping the mouse)
    positions = [(200,200),(400,300),(600,400),(800,500),(300,600),(900,200)]
    for (x,y) in positions:
        s.sendall(f'mouse_move {x} {y}\n'.encode())
        time.sleep(0.5)
    time.sleep(1)
    s.sendall(b'screendump ' + OUT.encode() + b'\n')
    time.sleep(2)
    s.sendall(b'quit\n')
    s.close()
    print('screendump issued', file=sys.stderr)
except Exception as e:
    print('monitor error:', repr(e), file=sys.stderr)
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()

# Analyze: count bright-white cursor pixels and cluster them to detect ghosting
from PIL import Image
from collections import defaultdict
img = Image.open(OUT).convert('RGB')
w,h = img.size
px = img.load()
white = []
for y in range(h):
    for x in range(w):
        r,g,b = px[x,y]
        if r>200 and g>200 and b>200:
            white.append((x,y))
print(f'white pixels: {len(white)}')
# group into clusters by proximity to detect multiple cursor ghosts
clusters = []
for (x,y) in white:
    placed=False
    for c in clusters:
        n=len(c['pts']); cx=c['x']//n; cy=c['y']//n
        if abs(cx-x)<40 and abs(cy-y)<40:
            c['pts'].append((x,y)); c['x']+=x; c['y']+=y; placed=True; break
    if not placed:
        clusters.append({'pts':[(x,y)],'x':x,'y':y})
print(f'white clusters (ghost detection): {len(clusters)}')
for i,c in enumerate(clusters[:10]):
    n=len(c['pts']); cx=c['x']//n; cy=c['y']//n
    print(f'  cluster {i}: center=({cx},{cy}) size={n}')
