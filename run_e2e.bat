@echo off
taskkill /f /im python.exe 2>nul
taskkill /f /im qemu-system-x86_64.exe 2>nul
ping -n 2 127.0.0.1 >nul
start "" cmd /c "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe D:\MyOS\bootloader\tools\nexos_bridge.py > D:\MyOS\bootloader\win11-ui\bridge_dbg.txt 2>&1"
ping -n 2 127.0.0.1 >nul
start "" D:\qemu\qemu-system-x86_64.exe -m 128 -accel tcg,tb-size=64 -display none -no-reboot -hda build/os_v2.img -serial tcp:127.0.0.1:4321,server
ping -n 6 127.0.0.1 >nul
echo === 4321 ===
netstat -an | findstr 4321
cd /d D:\MyOS\bootloader\win11-ui
echo === running LIVE E2E (agent console -> real kernel via bridge) ===
node _agent_live.mjs
echo === E2E done ===
