#!/usr/bin/env python3
import sys, time
sys.path.insert(0, 'tools')
import verify_gui as V

q = V.QEMU('build/os_v2.img', False, 140)
q.launch()
for i in range(40):
    time.sleep(2)
    im = q.capture('kp')
    if im and V.is_login_screen(im):
        print('login screen at', i); break
time.sleep(1)
for k in ['meta_l', 'up', 'down', 'left', 'right', 'tab']:
    q.key(k)
    time.sleep(0.4)
q.capture('after_keys')
time.sleep(0.5)
try: q.cmd('quit')
except Exception as e: print('quit err', e)
print('DONE')
