@echo off
taskkill /f /im python.exe 2>nul
taskkill /f /im qemu-system-x86_64.exe 2>nul
ping -n 2 127.0.0.1 >nul
start "" D:\qemu\qemu-system-x86_64.exe -m 250 -accel tcg,tb-size=128 -display none -no-reboot -hda build/os_v2.img -serial tcp:127.0.0.1:4321,server
ping -n 2 127.0.0.1 >nul
start "" cmd /c "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe -u D:\MyOS\bootloader\build\_diag_gui.py > D:\MyOS\bootloader\build\gui_diag.txt 2>&1"
echo launched diag in background; check build/gui_diag.txt after ~90s
