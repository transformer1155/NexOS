$ErrorActionPreference = 'Continue'
$qemu  = "D:\qemu\qemu-system-x86_64.exe"
$img   = "D:\MyOS\bootloader\build\os_v2.img"
$root  = "D:\MyOS\bootloader"
$py    = "python3"
$py310 = "C:\Users\trans\AppData\Local\Programs\Python\Python310\python.exe"
$node  = "node"

# 1. cleanup
taskkill /F /IM qemu-system-x86_64.exe 2>$null
taskkill /F /IM python3.exe 2>$null
taskkill /F /IM python.exe 2>$null
taskkill /F /IM node.exe 2>$null
Start-Sleep -Seconds 2

# 2. QEMU (named-pipe serial)
Start-Process -NoNewWindow $qemu -ArgumentList "-drive","format=raw,file=$img","-m","256","-display","none","-serial","pipe:nexos_serial","-accel","tcg" -RedirectStandardOutput "$root\qemu_dbg.txt" -RedirectStandardError "$root\qemu_err.txt"
Start-Sleep -Seconds 3

# 3. relay (Python310, win32file) pipe<->tcp 4399
Start-Process -NoNewWindow $py310 -ArgumentList "$root\nexos_relay.py" -RedirectStandardOutput "$root\relay_out.txt" -RedirectStandardError "$root\relay_err.txt"
Start-Sleep -Seconds 3

# 4. bridge (stdlib python3) -> relay tcp 4399
$env:SERIAL_PORT = "4399"
Start-Process -NoNewWindow $py -ArgumentList "$root\tools\nexos_bridge.py" -RedirectStandardOutput "$root\bridge_dbg.txt" -RedirectStandardError "$root\bridge_err.txt"
Start-Sleep -Seconds 3

# 5. http.server on 8000 (serving $root so /win11-ui/... resolves)
Start-Process -NoNewWindow $py -ArgumentList "-m","http.server","8000" -WorkingDirectory $root -RedirectStandardOutput "$root\http_dbg.txt" -RedirectStandardError "$root\http_err.txt"
Start-Sleep -Seconds 3

# 6. wait for kernel boot + GUI mode
Write-Host "=== waiting for kernel boot (~40s) ==="
Start-Sleep -Seconds 40

# 7. run E2E
Write-Host "=== running E2E (_e2e_ransom.mjs) ==="
Set-Location "$root\win11-ui"
& $node "_e2e_ransom.mjs" 2>&1 | Out-String -Width 400
Write-Host "=== E2E exited with code $LASTEXITCODE ==="
