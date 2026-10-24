#!/usr/bin/env python3
"""Quick right-click menu test for os_v2.img"""
import socket, time, sys, os

os.chdir(os.path.dirname(os.path.abspath(__file__)) + '/..')

def sock():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(10):
        try: s.connect('/tmp/mon.sock'); return s
        except: time.sleep(0.5)
    raise RuntimeError("no monitor")

def send(mon, cmd, delay=0.5):
    mon.sendall((cmd + '\n').encode())
    time.sleep(delay)

def screendump(path):
    mon = sock()
    send(mon, f'screendump {path}', 1)
    mon.close()

def wait_serial_marker(marker, timeout=30):
    start = time.time()
    while time.time() - start < timeout:
        try:
            with open('_qemu_serial.log') as f:
                data = f.read()
            if marker in data:
                print(f"  [SERIAL] Found: {marker}")
                return True
        except: pass
        time.sleep(1)
    print(f"  [SERIAL] MISSING: {marker}")
    return False

# 等 GUI ready
print("=== Waiting for shell.mex ready ===")
if not wait_serial_marker("[MFORMS] mforms: shell.mex ready", timeout=60):
    print("FAIL: shell.mex never loaded"); sys.exit(1)

# 截 base frame
print("=== Capturing base frame ===")
time.sleep(2)  # 让 desktop 稳定
screendump('_qemu_base.ppm')

# 发送右键事件 (桌面中央)
print("=== Sending right-click at (640, 400) ===")
mon = sock()
# 移动鼠标
send(mon, 'mouse_move 640 400', 0.3)
# 右键按下
send(mon, 'mouse button right', 0.2)
# 右键释放
send(mon, 'mouse button right', 0.5)
mon.close()

# 等菜单 render
time.sleep(3)
screendump('_qemu_menu.ppm')

# 验证
print("=== Analyzing frames ===")
import sys; sys.path.insert(0, 'tools')

def count_light(path, threshold=620):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    n = 0
    for i in range(0, w * h * 3, 3):
        if px[i] + px[i+1] + px[i+2] > threshold:
            n += 1
    return n, w, h

try:
    base, bw, bh = count_light('_qemu_base.ppm')
    menu, mw, mh = count_light('_qemu_menu.ppm')
    diff = menu - base
    print(f"  Base frame: {base} light pixels ({bw}x{bh})")
    print(f"  Menu frame: {menu} light pixels")
    print(f"  Delta: +{diff} light pixels")
    
    if diff > 8000:
        print("  PASS: right-click menu rendered!")
    elif diff > 0:
        print(f"  PARTIAL: only +{diff} light pixels (menu may be incomplete)")
    else:
        print("  FAIL: no menu detected (same or fewer light pixels)")
except Exception as e:
    print(f"  Analyze error: {e}")

print("Done")
