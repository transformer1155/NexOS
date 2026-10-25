#!/bin/bash
set -e
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log /tmp/mon.sock _t_*.ppm

IMG=$(pwd)/build/os_v2.img
QEMU=/usr/bin/qemu-system-x86_64

echo "=== Launching QEMU with $IMG ==="
$QEMU -drive format=raw,file=$IMG -m 4096 -vga std -display none   -serial file:_qemu_serial.log   -monitor unix:/tmp/mon.sock,server,nowait   -no-reboot &
QPID=$!
echo "QPID=$QPID"

echo "=== Wait 50s ==="
sleep 50

python3 -c "
import socket, time, os
CURSOR = [640, 360]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(20):
    try: s.connect('/tmp/mon.sock'); break
    except: time.sleep(0.3)
s.settimeout(2)
try: s.recv(65536)
except: pass

def send(c): s.sendall((c+'\n').encode()); time.sleep(0.15)

def move_abs(x, y):
    dx, dy = x-CURSOR[0], y-CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx))
        sy = max(-80, min(80, dy))
        send(f'mouse_move {sx} {sy}')
        CURSOR[0]+=sx; CURSOR[1]+=sy
        dx-=sx; dy-=sy
    time.sleep(0.25)

def click(x, y, btn):
    move_abs(x, y)
    send(f'mouse_button {btn}')
    time.sleep(0.5)
    send('mouse_button 0')
    time.sleep(0.3)

print('Dismiss...')
click(5, 5, 1)
print('Base frame...')
send('screendump _t_base.ppm')
time.sleep(2.5)
print('RIGHT-CLICK at (640,300)...')
click(640, 300, 2)
time.sleep(5)
print('Menu frame...')
send('screendump _t_menu.ppm')
time.sleep(2.5)
s.sendall(b'quit\n')
s.close()
print('Done')
"

kill $QPID 2>/dev/null
sleep 1

echo ""
echo "=== Serial debug lines ==="
grep -E "\[RF\]|\[GUI\].*RIGHT|DesktopRClick" _qemu_serial.log 2>/dev/null || echo "No debug lines!"
echo ""
echo "=== Frame files ==="
ls -la _t_base.ppm _t_menu.ppm 2>/dev/null || echo "FRAMES MISSING!"
