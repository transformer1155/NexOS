import subprocess, time, sys, os

QEMU = r'D:\qemu\qemu-system-x86_64.exe'
IMG  = r'D:\MyOS\bootloader\build\os_v2.img'
LOG  = r'D:\MyOS\bootloader\serial.log'

cmd = [
    QEMU, '-display', 'none',
    '-serial', f'file:{LOG}',
    '-drive', f'file={IMG},format=raw',
    '-m', '512', '-accel', 'tcg',
]
print('launching...', file=sys.stderr)
p = subprocess.Popen(cmd)
time.sleep(25)
p.terminate()
try: p.wait(timeout=5)
except Exception:
    p.kill()
print('done', file=sys.stderr)
if os.path.exists(LOG):
    print('--- serial log ---')
    with open(LOG, 'r', errors='replace') as f:
        lines = f.readlines()
    for line in lines[-40:]:
        print(line.rstrip())
else:
    print('NO serial.log')
