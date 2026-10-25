#!/usr/bin/env python3
"""Full right-click menu test for os_v2.img"""
import socket, time, os, sys

os.chdir('/mnt/e/NexOS/NexOS')

def sock_connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(20):
        try: s.connect('/tmp/mon.sock'); return s
        except: time.sleep(0.5)
    raise RuntimeError("monitor not ready")

def cmd(mon, c, delay=0.3):
    mon.sendall((c + '\n').encode())
    time.sleep(delay)

def count_light(path, threshold=620):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6', "bad ppm header"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    n = 0
    for i in range(0, len(px), 3):
        if px[i] + px[i+1] + px[i+2] > threshold:
            n += 1
    return n, w, h

# 读串口
def tail_log(n=20):
    try:
        with open('_qemu_serial.log') as f:
            lines = f.readlines()
        return lines[-n:]
    except: return []

print("=== Phase 0: wait for GUI ===")
start = time.time()
while time.time() - start < 60:
    log = ''.join(tail_log(50))
    if '[MFORMS] mforms: shell.mex ready' in log:
        print("  shell.mex loaded OK")
        break
    time.sleep(1)
else:
    print("FAIL: shell.mex never loaded"); sys.exit(1)

time.sleep(3)  # desktop 稳定

# dismiss 可能的菜单
mon = sock_connect()
print("=== Phase 1: dismiss any menu ===")
# mouse_move 是 ABSOLUTE (QEMU 8.x)
cmd(mon, 'mouse_move 5 5', 0.3)
cmd(mon, 'mouse_button 1', 0.2)   # left down
cmd(mon, 'mouse_button 0', 0.3)   # up
cmd(mon, 'screendump _qemu_base.ppm', 1)
print("  base frame captured")

print("=== Phase 2: right-click on wallpaper (640, 300) ===")
cmd(mon, 'mouse_move 640 300', 0.3)
cmd(mon, 'mouse_button 2', 0.3)   # right down
cmd(mon, 'mouse_button 0', 0.5)   # up

# 等 render
time.sleep(4)
cmd(mon, 'screendump _qemu_menu.ppm', 1)
mon.close()

print("  menu frame captured")

# 分析
print("\n=== Phase 3: analyze ===")
base_n, bw, bh = count_light('_qemu_base.ppm')
menu_n, mw, mh = count_light('_qemu_menu.ppm')
diff = menu_n - base_n

print(f"  Frame: {bw}x{bh}")
print(f"  Base light px: {base_n}")
print(f"  Menu light px: {menu_n}")
print(f"  Delta: +{diff}")

if diff > 15000:
    print("  PASS: right-click menu rendered successfully!")
elif diff > 5000:
    print(f"  PARTIAL: menu partially visible (+{diff})")
elif diff > 0:
    print(f"  WEAK: only +{diff} - menu may be broken")
else:
    print(f"  FAIL: no menu detected (diff={diff})")
