import subprocess, socket, time, struct, sys
from PIL import Image
from collections import defaultdict

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os_v2.img'
OUT  = r'D:\MyOS\bootloader\shot_vnc.ppm'
MON  = ('127.0.0.1', 1236)
VNC  = ('127.0.0.1', 5901)

def vnc_connect():
    s = socket.create_connection(VNC, timeout=10)
    # Server version
    ver = s.recv(12).decode('ascii')
    if not ver.startswith('RFB '):
        raise RuntimeError('not RFB: ' + ver)
    s.sendall(ver.encode('ascii'))
    # Security types
    sec = s.recv(1)[0]
    if sec == 0:
        raise RuntimeError('no security types')
    types = list(s.recv(sec))
    if 1 not in types:
        raise RuntimeError('NoAuth not offered: ' + str(types))
    s.sendall(bytes([1]))  # choose None
    # Security result
    res = struct.unpack('>I', s.recv(4))[0]
    if res != 0:
        raise RuntimeError('security failed: ' + str(res))
    # ClientInit (non-shared)
    s.sendall(bytes([0]))
    # ServerInit
    init = s.recv(24)
    w, h, pf16, namelen = struct.unpack('>HH16sI', init)
    s.recv(namelen)
    # Request a full update once to make server happy (optional)
    # FramebufferUpdateRequest: type=3, incremental=0, x=0,y=0,w,h
    s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, w, h))
    time.sleep(0.5)
    # drain update rectangles
    s.settimeout(2)
    try:
        while True:
            hdr = s.recv(1)
            if not hdr: break
            if hdr[0] == 0:
                # FramebufferUpdate: pad(1) + nrects(2)
                s.recv(3)
                n = struct.unpack('>H', s.recv(2))[0]
                for _ in range(n):
                    s.recv(12)  # header
                    enc = struct.unpack('>i', s.recv(4))[0]
                    if enc == 0:
                        rw = struct.unpack('>H', s.recv(2))[0]
                        rh = struct.unpack('>H', s.recv(2))[0]
                        s.recv(rw * rh * 4)
            else:
                s.recv(1024)
    except socket.timeout:
        pass
    s.settimeout(10)
    return s, w, h

def vnc_mouse(s, x, y):
    # PointerEvent: type=5, button-mask, x, y (big-endian)
    s.sendall(struct.pack('>BBHH', 5, 0, x, y))

cmd = [
    QEMU, '-vnc', ':1', '-vga', 'std',
    '-monitor', f'tcp:{MON[0]}:{MON[1]},server,nowait',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
p = subprocess.Popen(cmd)
time.sleep(16)

# Connect VNC and sweep the mouse
vs, vw, vh = vnc_connect()
positions = [(100,100),(300,150),(500,250),(700,350),(500,450),(300,550),(100,650),(400,400),(800,200),(900,500)]
for (x,y) in positions:
    vnc_mouse(vs, x, y)
    time.sleep(0.25)
vs.close()

# Screendump via QEMU monitor
try:
    ms = socket.create_connection(MON, timeout=5)
    time.sleep(0.5)
    ms.sendall(b'screendump ' + OUT.encode() + b'\n')
    time.sleep(2)
    ms.sendall(b'quit\n'); ms.close()
except Exception as e:
    print('monitor err', repr(e), file=sys.stderr)
p.terminate()
try: p.wait(timeout=5)
except Exception: p.kill()

# Analyze clusters of bright pixels (cursor arrows)
im = Image.open(OUT).convert('RGB')
w,h = im.size
white = []
for y in range(h):
    for x in range(w):
        r,g,b = im.getpixel((x,y))
        if r>200 and g>200 and b>200:
            white.append((x,y))
print(f'white pixels: {len(white)}')
clusters = []
for (x,y) in white:
    placed=False
    for c in clusters:
        n=len(c['pts']); cx=c['x']//n; cy=c['y']//n
        if abs(cx-x)<50 and abs(cy-y)<50:
            c['pts'].append((x,y)); c['x']+=x; c['y']+=y; placed=True; break
    if not placed:
        clusters.append({'pts':[(x,y)],'x':x,'y':y})
print(f'white clusters: {len(clusters)}')
for i,c in enumerate(clusters[:15]):
    n=len(c['pts']); cx=c['x']//n; cy=c['y']//n
    print(f'  cluster {i}: center=({cx},{cy}) size={n}')

# Save small PNG for visual check
im.resize((320,180)).save('shot_vnc_small.png')
