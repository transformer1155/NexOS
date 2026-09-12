# NexOS (妮可OS) GUI interaction + animation verification driver.
# Boots the OS headless under QEMU, drives it like a real user over QMP
# (keyboard + relative mouse), and captures a timeline of screenshots so the
# window open / minimize / restore / snap / start-menu animations can be
# inspected frame-by-frame.
#
# Usage:
#   .\verify_gui.ps1
# Output: D:\MyOS\bootloader\build\vx\*.png

param(
    [string]$QemuDir = "D:\qemu",
    [string]$Img     = "D:\MyOS\bootloader\build\os_v2.img",
    [int]   $Mem     = 1024,
    [int]   $BootWait= 20,
    [int]   $QmpPort = 1241,
    [string]$OutDir  = "D:\MyOS\bootloader\build\vx"
)

$ErrorActionPreference = "Continue"
$qemu = Join-Path $QemuDir "qemu-system-x86_64.exe"
if (-not (Test-Path $qemu)) { Write-Host "[ERROR] QEMU not found: $qemu" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Img))  { Write-Host "[ERROR] Image not found: $Img" -ForegroundColor Red; exit 1 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---- QMP helper -----------------------------------------------------------
$qmpSock = $null
function Qmp-Connect {
    param([int]$Port)
    $script:qmpSock = New-Object System.Net.Sockets.TcpClient
    $script:qmpSock.Connect("127.0.0.1", $Port)
    $script:qmpStream = $script:qmpSock.GetStream()
    # drain greeting
    Start-Sleep -Milliseconds 300
    $buf = New-Object byte[] 4096
    $n = $script:qmpStream.Read($buf, 0, $buf.Length)
    $greet = [Text.Encoding]::ASCII.GetString($buf, 0, $n)
    # negotiate capabilities
    Qmp-Send '{"execute":"qmp_capabilities"}' | Out-Null
    return $greet
}
function Qmp-Send {
    param([string]$Json)
    $b = [Text.Encoding]::ASCII.GetBytes($Json + "`n")
    $script:qmpStream.Write($b, 0, $b.Length)
    $script:qmpStream.Flush()
    Start-Sleep -Milliseconds 120
    # read the reply (one JSON line)
    $buf = New-Object byte[] 8192
    $out = ""
    while ($script:qmpStream.DataAvailable) {
        $n = $script:qmpStream.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $out += [Text.Encoding]::ASCII.GetString($buf, 0, $n)
    }
    return $out
}
function Qmp-Shot {
    param([string]$Name)
    $ppm = Join-Path $OutDir ($Name + ".ppm")
    $png = Join-Path $OutDir ($Name + ".png")
    if (Test-Path $ppm) { Remove-Item $ppm -Force }
    $ppmj = $ppm.Replace('\', '\\')
    $reply = Qmp-Send "{`"execute`":`"screendump`",`"arguments`":{`"filename`":`"$ppmj`"}}"
    if ($reply -match '"error"') { Write-Host "  [screendump reply] $reply" -ForegroundColor Yellow }
    Start-Sleep -Milliseconds 600
    # convert PPM -> PNG via python (no PIL needed)
    $py = @"
import sys,struct,zlib
d=open(r'$ppm','rb').read()
assert d[:2]==b'P6'
i=2;parts=[]
while len(parts)<3:
    while d[i] in b' \t\n\r': i+=1
    s=i
    while d[i] not in b' \t\n\r': i+=1
    parts.append(int(d[s:i])); i+=1
w,h,mx=parts
px=d[i:]
def chunk(t,b):
    c=t+b; return struct.pack('>I',len(b))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
raw=bytearray()
for y in range(h):
    raw.append(0); raw.extend(px[y*w*3:(y+1)*w*3])
sig=b'\x89PNG\r\n\x1a\n'
ihdr=struct.pack('>IIBBBBB',w,h,8,2,0,0,0)
idat=zlib.compress(bytes(raw),6)
open(r'$png','wb').write(sig+chunk(b'IHDR',ihdr)+chunk(b'IDAT',idat)+chunk(b'IEND',b''))
print(w,h)
"@
    $r = python -c $py 2>&1
    Write-Host "  shot $Name -> $png  ($r)" -ForegroundColor Gray
    return $r
}

# ---- input helpers --------------------------------------------------------
$CurX = 0; $CurY = 0
function Qmp-MoveAbs {
    param([int]$X, [int]$Y)
    $dx = $X - $script:CurX; $dy = $Y - $script:CurY
    $script:CurX = $X; $script:CurY = $Y
    Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"rel`",`"data`":{`"axis`":`"x`",`"value`":$dx}},{`"type`":`"rel`",`"data`":{`"axis`":`"y`",`"value`":$dy}}]}}" | Out-Null
}
function Qmp-Click {
    Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"btn`",`"data`":{`"button`":`"left`",`"down`":true}}]}}" | Out-Null
    Start-Sleep -Milliseconds 30
    Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"btn`",`"data`":{`"button`":`"left`",`"down`":false}}]}}" | Out-Null
}
function Qmp-Drag {
    param([int]$X1,[int]$Y1,[int]$X2,[int]$Y2)
    Qmp-MoveAbs $X1 $Y1
    Start-Sleep -Milliseconds 60
    Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"btn`",`"data`":{`"button`":`"left`",`"down`":true}}]}}" | Out-Null
    # move in steps so the window-drag handler (fires every 3rd move) updates
    $steps = 8
    for ($s = 1; $s -le $steps; $s++) {
        $ix = $X1 + ($X2 - $X1) * $s / $steps
        $iy = $Y1 + ($Y2 - $Y1) * $s / $steps
        Qmp-MoveAbs $ix $iy
        Start-Sleep -Milliseconds 25
    }
    Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"btn`",`"data`":{`"button`":`"left`",`"down`":false}}]}}" | Out-Null
}
function Qmp-Key {
    param([string]$Q)
    Qmp-Send "{`"execute`":`"send-key`",`"arguments`":{`"keys`":[{`"type`":`"qcode`",`"data`":`"$Q`"}]}}" | Out-Null
}
function Qmp-Type {
    param([string]$Str)
    foreach ($ch in $Str.ToCharArray()) {
        Qmp-Key ([string]$ch)
        Start-Sleep -Milliseconds 40
    }
}

# ---- launch QEMU ----------------------------------------------------------
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  NexOS GUI verification (QMP driver)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
$args = @(
    "-drive", "format=raw,file=$Img",
    "-m", "$Mem",
    "-vga", "std",
    "-display", "none",
    "-machine", "pc,mem-merge=off",
    "-accel", "tcg",
    "-no-reboot",
    "-serial", "file:$(Join-Path $OutDir 'serial.log')",
    "-chardev", "socket,id=qmp0,server=on,wait=off,host=127.0.0.1,port=$QmpPort",
    "-mon", "chardev=qmp0,mode=control"
)
Write-Host "[1] Booting QEMU (headless, QMP on :$QmpPort)..." -ForegroundColor Yellow
$qerr = Join-Path $OutDir "qemu.err"
$p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -WindowStyle Hidden -RedirectStandardError $qerr
# Wait until the QMP socket is accepting connections (TCG boot is slow).
$ready = $false
for ($i = 0; $i -lt ($BootWait + 10); $i++) {
    Start-Sleep -Seconds 1
    try {
        $t = New-Object System.Net.Sockets.TcpClient
        $t.Connect("127.0.0.1", $QmpPort)
        $t.Close()
        $ready = $true
        Write-Host "  QMP socket listening after ~$i s" -ForegroundColor Gray
        break
    } catch { }
}
if (-not $ready) {
    Write-Host "[ERROR] QMP never came up. QEMU stderr:" -ForegroundColor Red
    if (Test-Path $qerr) { Get-Content $qerr | Select-Object -Last 20 }
    if (-not $p.HasExited) { $p.Kill() }
    exit 1
}

$greet = Qmp-Connect -Port $QmpPort
Write-Host "  QMP greeting bytes: $($greet.Length)" -ForegroundColor Gray

# resolution from first screenshot
Write-Host "[2] Login screen..." -ForegroundColor Yellow
$r = Qmp-Shot "01_lock"
# parse W,H
$m = [regex]::Match($r, '(\d+)\s+(\d+)')
$W = [int]$m.Groups[1].Value; $H = [int]$m.Groups[2].Value
Write-Host "  Resolution: ${W}x${H}" -ForegroundColor Cyan

# taskbar geometry
$GroupW = 362            # (7 pins + 1 start) * 40 + 7 * 6
$bx = [int](($W - $GroupW) / 2)
$by = $H - 44           # h-48 + (48-40)/2
Write-Host "  Start btn at x=$bx y=$by" -ForegroundColor Gray

# ---- login: pre-filled user is 'root', must switch to demo account 'nexos' first ----
Write-Host "[3] Login (switch to nexos, type password, Enter)..." -ForegroundColor Yellow
$cardW = 400; $cardH = 430
$cardX = [int](($W - $cardW) / 2)
$cardY = [int](($H - $cardH) / 2) + 20
$fieldW = $cardW - 96
$fieldX = $cardX + 48
$chipY = $cardY + 364
$chipCw = [int](($fieldW - 16) / 3)   # 96 px for 3 chips
# chips: root, guest, nexos; click the third chip
$nexosChipX = $fieldX + 2 * ($chipCw + 8) + [int]($chipCw / 2)
$nexosChipY = $chipY + 14
Qmp-MoveAbs $nexosChipX $nexosChipY
Start-Sleep -Milliseconds 80
Qmp-Click
Start-Sleep -Milliseconds 300
Qmp-Type "nexos"
Start-Sleep -Milliseconds 200
Qmp-Key "ret"
Start-Sleep -Seconds 2
Qmp-Shot "02_desktop"

# ---- Start menu open/close ----
Write-Host "[4] Start menu open (click Start)..." -ForegroundColor Yellow
Qmp-MoveAbs ($bx + 20) ($by + 20)
Start-Sleep -Milliseconds 80
Qmp-Click
Start-Sleep -Milliseconds 90
Qmp-Shot "03_start_mid"
Start-Sleep -Milliseconds 320
Qmp-Shot "04_start_open"

Write-Host "[5] Start menu close (click desktop)..." -ForegroundColor Yellow
Qmp-MoveAbs ([int]($W/2)) ([int]($H/2))
Start-Sleep -Milliseconds 60
Qmp-Click
Start-Sleep -Milliseconds 90
Qmp-Shot "05_start_closing"
Start-Sleep -Milliseconds 300
Qmp-Shot "06_desktop2"

# ---- open an app (Calculator, pin index 2) ----
$pinX = $bx + (2 + 1) * 46 + 20   # bx + 3*46 + 20
Write-Host "[6] Open Calculator (taskbar pin 2)..." -ForegroundColor Yellow
Qmp-MoveAbs $pinX ($by + 20)
Start-Sleep -Milliseconds 80
Qmp-Click
Start-Sleep -Milliseconds 110
Qmp-Shot "07_winopen_mid"
Start-Sleep -Milliseconds 420
Qmp-Shot "08_winopen"

# ---- minimize via taskbar (click same pin) ----
Write-Host "[7] Minimize (click pin again)..." -ForegroundColor Yellow
Qmp-MoveAbs $pinX ($by + 20)
Start-Sleep -Milliseconds 60
Qmp-Click
Start-Sleep -Milliseconds 110
Qmp-Shot "09_min_mid"
Start-Sleep -Milliseconds 420
Qmp-Shot "10_min_done"

# ---- restore via taskbar ----
Write-Host "[8] Restore (click pin again)..." -ForegroundColor Yellow
Qmp-MoveAbs $pinX ($by + 20)
Start-Sleep -Milliseconds 60
Qmp-Click
Start-Sleep -Milliseconds 110
Qmp-Shot "11_restore_mid"
Start-Sleep -Milliseconds 420
Qmp-Shot "12_restore_done"

# ---- drag to snap (left half) ----
Write-Host "[9] Drag title to left edge (snap)..." -ForegroundColor Yellow
$titleX = [int]($W / 2)
$titleY = [int]($H * 0.22)
Qmp-Drag $titleX $titleY 12 $titleY
Start-Sleep -Milliseconds 200
Qmp-Shot "13_snap"

# ---- quit ----
Write-Host "[10] Quitting..." -ForegroundColor Yellow
Qmp-Send '{"execute":"quit"}' | Out-Null
Start-Sleep -Seconds 2
if (-not $p.HasExited) { $p.Kill() }
$p.WaitForExit(5000) | Out-Null
Write-Host "[DONE] Screenshots in $OutDir" -ForegroundColor Green
Get-ChildItem $OutDir -Filter *.png | ForEach-Object { Write-Host "  $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB" }
