@echo off
taskkill /f /im qemu-system-x86_64.exe 2>nul
taskkill /f /im python.exe 2>nul
ping -n 3 127.0.0.1 >nul
REM start the bridge FIRST so it is the sole client accepted by QEMU's serial socket
start "" C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe D:\MyOS\bootloader\tools\nexos_bridge.py
ping -n 3 127.0.0.1 >nul
start "" D:\qemu\qemu-system-x86_64.exe -m 128 -drive file=build/os_v2.img,format=raw,if=ide -boot c -display none -no-reboot -accel tcg,tb-size=128 -serial tcp:127.0.0.1:4321,server,nowait
ping -n 9 127.0.0.1 >nul
echo QEMU+bridge started (bridge first).
