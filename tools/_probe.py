import socket, time, subprocess, sys, os

# Just boot QEMU and let bridge sit idle, log bridge output for 25s, no commands.
print("[idle-probe] starting QEMU (headless) ...")
qemu = subprocess.Popen(
    ['D:\\qemu\\qemu-system-x86_64.exe',
     '-drive', 'file=\\MyOS\\bootloader\\build\\os_v2.img,format=raw',
     '-chardev', 'socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=4321',
     '-serial', 'chardev:ser0', '-display', 'none', '-vga', 'std',
     '-machine', 'pc,mem-merge=off', '-m', '512', '-accel', 'tcg', '-no-reboot'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)
bridge = subprocess.Popen(
    [sys.executable, 'D:\\MyOS\\Bootloader\\tools\\nexos_bridge.py'],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
print("[idle-probe] bridge started, sleeping 25s idle (no commands)...")
t0 = time.time()
while time.time() - t0 < 25:
    line = bridge.stdout.readline()
    if not line:
        break
    print("[bridge] " + line.rstrip())
    if time.time() - t0 > 25:
        break
qemu.terminate(); bridge.terminate()
print("[idle-probe] done")
