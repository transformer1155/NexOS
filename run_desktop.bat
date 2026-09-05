@echo off
title NexOS Desktop (double-click to run)
chcp 65001 >nul

REM --- kill leftovers ---
taskkill /f /im python.exe 2>nul
taskkill /f /im qemu-system-x86_64.exe 2>nul
ping -n 2 127.0.0.1 >nul

REM --- start bridge (WebSocket 8765 <-> serial 4321) ---
start "" cmd /c "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe -u D:\MyOS\bootloader\tools\nexos_bridge.py > D:\MyOS\bootloader\win11-ui\bridge_dbg.txt 2>&1"
ping -n 2 127.0.0.1 >nul

REM --- start real kernel VM ---
start "" D:\qemu\qemu-system-x86_64.exe -m 250 -accel tcg,tb-size=128 -display none -no-reboot -hda build/os_v2.img -serial tcp:127.0.0.1:4321,server

REM --- wait for kernel to reach GUI mode ---
echo Waiting ~45s for guest to boot to GUI ...
ping -n 46 127.0.0.1 >nul

REM --- sanity: bridge listening on 8765? ---
echo === 8765 (bridge) ===
netstat -an | findstr 8765
echo === 4321 (kernel serial) ===
netstat -an | findstr 4321

REM --- serve the project root over HTTP (file:// blocks WS to 127.0.0.1) ---
echo Serving NexOS desktop over http://127.0.0.1:8000 ...
start "" cmd /c "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe -u -m http.server 8000 --directory D:\MyOS\bootloader"
ping -n 2 127.0.0.1 >nul

REM --- open the frontend in default browser via http:// ---
echo Opening NexOS desktop ...
start "" "http://127.0.0.1:8000/win11-ui/nexos-desktop.html"

echo.
echo ============================================================
echo  NexOS is running.
echo   - Frontend:  the browser window just opened
echo   - Login:     nexos / nexos
echo   - AI Desktop (4th tile) -> Distributed Network app
echo       * click Start  -> agent running
echo       * click Scan   -> discover host node 10.0.2.2
echo  Bridge log: D:\MyOS\bootloader\win11-ui\bridge_dbg.txt
echo  Close this window to keep running; kill via Task Manager.
echo ============================================================
echo.
echo Press any key to open bridge log viewer (tail) ...
pause >nul
powershell -command "Get-Content D:\MyOS\bootloader\win11-ui\bridge_dbg.txt -Tail 20 -Wait"
