@echo off
taskkill /f /im qemu-system-x86_64.exe 2>nul
taskkill /f /im python.exe 2>nul
ping -n 2 127.0.0.1 >nul
REM Launch the serial client FIRST so it is already retrying-connect when QEMU
REM brings up the tcp serial (nowait => QEMU does not block; client attaches early)
start "" C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe D:\MyOS\bootloader\win11-ui\_diag_serial2.py > D:\MyOS\bootloader\win11-ui\diag_out.txt 2>&1
ping -n 2 127.0.0.1 >nul
start "" D:\qemu\qemu-system-x86_64.exe -m 4096 -accel tcg,tb-size=128 -display none -no-reboot -hda build/os_v2.img -serial tcp:127.0.0.1:4321,server,nowait
echo waiting 40s for boot + agent verification...
ping -n 40 127.0.0.1 >nul
echo === diag_out.txt ===
type D:\MyOS\bootloader\win11-ui\diag_out.txt
