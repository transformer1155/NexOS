#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log /tmp/mon.sock _v_*.ppm

/usr/bin/qemu-system-x86_64 \
  -drive format=raw,file=$(pwd)/build/os_v2.img \
  -m 4096 -vga std -display none \
  -serial file:_qemu_serial.log \
  -monitor unix:/tmp/mon.sock,server,nowait \
  -no-reboot &
QPID=$!
echo "QPID=$QPID"

sleep 50

python3 -c "
import socket, time
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
        sx = max(-80, min(80, dx)); sy = max(-80, min(80, dy))
        send(f'mouse_move {sx} {sy}')
        CURSOR[0]+=sx; CURSOR[1]+=sy; dx-=sx; dy-=sy
    time.sleep(0.25)

def click(x, y, btn, hold=0.4):
    move_abs(x, y)
    send(f'mouse_button {btn}')
    time.sleep(hold)
    send('mouse_button 0')
    time.sleep(0.3)

# dismiss + base
click(5, 5, 1)
send('screendump _v_0_base.ppm'); time.sleep(2.5)

# --- 场景1: 右键菜单打开后完全不动鼠标, 等 3 秒让 keepalive 兜底 ---
print('S1: right-click then FREEZE 3s...')
click(640, 300, 2)
time.sleep(3.5)
send('screendump _v_1_menu_frozen.ppm'); time.sleep(2.5)

# --- 场景2: 菜单开着, hover 移动 (重影检测) ---
print('S2: hover over menu...')
move_abs(700, 340); time.sleep(0.5)
move_abs(680, 360); time.sleep(0.5)
send('screendump _v_2_hover.ppm'); time.sleep(2.5)

# --- 场景3: 长按左键 1.5s (重影检测) ---
print('S3: long-press 1.5s...')
send('mouse_button 0')  # release anything
move_abs(400, 500); time.sleep(0.4)
send('mouse_button 1')
time.sleep(1.5)
send('screendump _v_3_pressed.ppm'); time.sleep(2)
send('mouse_button 0'); time.sleep(0.8)
send('screendump _v_4_released.ppm'); time.sleep(2.5)

s.sendall(b'quit\n'); s.close()
print('captures done')
"

kill $QPID 2>/dev/null; sleep 1

echo ""
echo "=== Serial check ==="
grep -c "GUI" _qemu_serial.log | head -1
ls -la _v_*.ppm 2>/dev/null || echo FRAMES_MISSING
