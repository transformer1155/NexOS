@echo off
taskkill /f /im python.exe 2>nul
taskkill /f /im qemu-system-x86_64.exe 2>nul
ping -n 2 127.0.0.1 >nul
start "" cmd /c "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe D:\MyOS\bootloader\tools\nexos_bridge.py > D:\MyOS\bootloader\win11-ui\bridge_dbg.txt 2>&1"
ping -n 2 127.0.0.1 >nul
start "" D:\qemu\qemu-system-x86_64.exe -m 250 -accel tcg,tb-size=128 -display none -no-reboot -hda build/os_v2.img -serial tcp:127.0.0.1:4321,server
echo waiting 75s for guest to reach GUI mode (no browser)...
ping -n 76 127.0.0.1 >nul
echo === bridge tail (expect NO serial link lost if bridge-alone is stable) ===
powershell -command "Get-Content D:\MyOS\bootloader\win11-ui\bridge_dbg.txt -Tail 12"
