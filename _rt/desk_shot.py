#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Custom driver v2: select the 'nexos' account chip, log in, open a window."""
import sys, time
sys.path.insert(0, 'tools')
import verify_gui as V

q = V.QEMU('build/os_v2.img', False, 240)
q.launch()

def wait_login(t=90):
    end = time.time() + t
    while time.time() < end:
        time.sleep(2)
        im = q.capture('ls')
        if im and V.is_login_screen(im):
            return im
    return None

im = wait_login()
if not im:
    print('NO LOGIN SCREEN'); q.cmd('quit'); sys.exit(1)
W, H, _ = im
print('login screen %dx%d' % (W, H))

# 1) select the 'nexos' account chip (bottom row, 3rd chip)
q.click(int(W*0.585), int(H*0.905)); time.sleep(0.6)
q.capture('after_chip')
# 2) password field
q.click(int(W*0.50), int(H*0.69)); time.sleep(0.3)
q.type_text('nexos'); time.sleep(0.4)
# 3) login button
q.click(int(W*0.50), int(H*0.77)); time.sleep(1.5)

desk = None
for i in range(22):
    time.sleep(1.0)
    c = q.capture('dl%d' % i)
    if c and not V.is_login_screen(c):
        print('desktop at attempt', i); desk = c; break
if not desk:
    print('LOGIN STILL FAILED')
    try: q.capture('fail_final')
    except Exception as e: print('capture fail:', e)
    q.cmd('quit'); sys.exit(2)
q.capture('desktop_ok')

def dblclick(x, y):
    q.click(x, y); time.sleep(0.08); q.click(x, y); time.sleep(1.3)

# open a window: try desktop icons then the Start button
for (cx, cy) in [(60, 80), (60, 150), (60, 220), (60, 290)]:
    try: dblclick(cx, cy)
    except Exception as e:
        print('dbl err', e); break
q.capture('win_final')

try: q.cmd('quit')
except Exception: pass
print('DONE')
