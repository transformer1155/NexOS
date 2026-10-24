#!/usr/bin/env python3
"""100% correct QEMU right-click test"""
import socket, time, os, sys

os.chdir('/mnt/e/NexOS/NexOS')

CURSOR = [640, 360]  # 起始位置 (kernel 默认)

def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(20):
        try: s.connect('/tmp/mon.sock'); break
        except: time.sleep(0.3)
    s.settimeout(2)
    try: s.recv(65536)  # 吞掉 monitor banner
    except: pass
    return s

def move_abs(mon, x, y):
    """相对移动，每次最多 80px"""
    global CURSOR
    dx, dy = x - CURSOR[0], y - CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx))
        sy = max(-80, min(80, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.12)
        CURSOR[0] += sx; CURSOR[1] += sy
        dx -= sx; dy -= sy
    time.sleep(0.25)

def press(mon, btn):
    """mouse_button 1=left, 2=right, 0=up"""
    mon.sendall(("mouse_button %d\n" % btn).encode())
    time.sleep(0.2)

def left_click(mon, x, y):
    move_abs(mon, x, y)
    press(mon, 1); time.sleep(0.15)
    press(mon, 0); time.sleep(0.3)

def right_click(mon, x, y):
    move_abs(mon, x, y)
    press(mon, 2); time.sleep(0.6)   # 按住更久!
    press(mon, 0); time.sleep(0.4)

def screendump(mon, path):
    mon.sendall(("screendump %s\n" % path).encode())
    time.sleep(2.5)

print("=== Launching QEMU (done externally, assuming running) ===")
print("Waiting 45s...")
time.sleep(45)

print("=== Connect ===")
mon = connect()

# 确认 shell.mex 已加载
start = time.time()
while time.time() - start < 20:
    try:
        with open('_qemu_serial.log') as f: data = f.read()
        if 'mforms: shell.mex ready' in data:
            print("  shell.mex ready"); break
    except: pass
    time.sleep(1)

# dismiss 可能的菜单
print("=== Dismiss any menu ===")
left_click(mon, 5, 5)
time.sleep(0.5)

# capture base
print("=== Base frame ===")
screendump(mon, '_t_base.ppm')

# 右键!
print("=== RIGHT-CLICK at (640, 300) wallpaper ===")
right_click(mon, 640, 300)

# 等 render + 动画
time.sleep(5)

# capture menu
print("=== Menu frame ===")
screendump(mon, '_t_menu.ppm')

mon.close()
print("Done")
