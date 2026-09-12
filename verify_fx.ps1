# Focused E2E test for the File Explorer address bar (managed C# layer).
# Boots NexOS headless, signs in as the demo account, opens File Explorer,
# then exercises the new editable address bar:
#   - invalid address -> "你输入的地址无效" dialog + reset to default view
#   - valid address   -> navigate to that volume, address box updates
# Output: D:\MyOS\bootloader\build\fx\*.png

param(
    [string]$QemuDir = "D:\qemu",
    [string]$Img     = "D:\MyOS\bootloader\build\os_v2.img",
    [int]   $Mem     = 1024,
    [int]   $BootWait= 22,
    [int]   $QmpPort = 1243,
    [string]$OutDir  = "D:\MyOS\bootloader\build\fx"
)
$ErrorActionPreference = "Continue"
$qemu = Join-Path $QemuDir "qemu-system-x86_64.exe"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$qmpSock=$null;$qmpStream=$null
function Qmp-Connect { param([int]$Port)
    $script:qmpSock = New-Object System.Net.Sockets.TcpClient
    $script:qmpSock.Connect("127.0.0.1", $Port)
    $script:qmpStream = $script:qmpSock.GetStream()
    Start-Sleep -Milliseconds 300
    $b = New-Object byte[] 4096
    $n = $script:qmpStream.Read($b,0,$b.Length)
    [void](Qmp-Send '{"execute":"qmp_capabilities"}')
    return $n
}
function Qmp-Send { param([string]$Json)
    $b=[Text.Encoding]::ASCII.GetBytes($Json+"`n")
    try { $script:qmpStream.Write($b,0,$b.Length); $script:qmpStream.Flush() } catch { return "" }
    Start-Sleep -Milliseconds 110
    $buf=New-Object byte[] 8192; $out=""
    while ($script:qmpStream.DataAvailable) { $n=$script:qmpStream.Read($buf,0,$buf.Length); if($n -le 0){break}; $out+=[Text.Encoding]::ASCII.GetString($buf,0,$n) }
    return $out
}
function Qmp-Shot { param([string]$Name)
    $ppm=Join-Path $OutDir ($Name+".ppm"); $png=Join-Path $OutDir ($Name+".png")
    if (Test-Path $ppm) { Remove-Item $ppm -Force }
    $ppmj=$ppm.Replace('\','\\')
    [void](Qmp-Send "{`"execute`":`"screendump`",`"arguments`":{`"filename`":`"$ppmj`"}}")
    Start-Sleep -Milliseconds 600
    $py=@"
import struct,zlib
d=open(r'$ppm','rb').read()
i=2;parts=[]
while len(parts)<3:
    while d[i] in b' \t\n\r': i+=1
    s=i
    while d[i] not in b' \t\n\r': i+=1
    parts.append(int(d[s:i])); i+=1
w,h,mx=parts; px=d[i:]
def ck(t,b):
    c=t+b; return struct.pack('>I',len(b))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
raw=bytearray()
for y in range(h):
    raw.append(0); raw.extend(px[y*w*3:(y+1)*w*3])
open(r'$png','wb').write(b'\x89PNG\r\n\x1a\n'+ck(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ck(b'IDAT',zlib.compress(bytes(raw),6))+ck(b'IEND',b''))
print(w,h)
"@
    $r = python -c $py 2>&1
    Write-Host "  shot $Name ($r)" -ForegroundColor Gray
    return $r
}
$CurX=0;$CurY=0
function Qmp-MoveAbs { param([int]$X,[int]$Y)
    $dx=$X-$script:CurX; $dy=$Y-$script:CurY; $script:CurX=$X; $script:CurY=$Y
    [void](Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"rel`",`"data`":{`"axis`":`"x`",`"value`":$dx}},{`"type`":`"rel`",`"data`":{`"axis`":`"y`",`"value`":$dy}}]}}")
}
function Qmp-Btn { param([bool]$Down)
    $d = if($Down){"true"}else{"false"}
    [void](Qmp-Send "{`"execute`":`"input-send-event`",`"arguments`":{`"events`":[{`"type`":`"btn`",`"data`":{`"button`":`"left`",`"down`":$d}}]}}")
}
function Qmp-Click { Qmp-Btn $true; Start-Sleep -Milliseconds 30; Qmp-Btn $false }
function Qmp-Key { param([string]$Q)
    [void](Qmp-Send "{`"execute`":`"send-key`",`"arguments`":{`"keys`":[{`"type`":`"qcode`",`"data`":`"$Q`"}]}}")
}
function Qmp-Type { param([string]$Str) foreach($ch in $Str.ToCharArray()){ Qmp-Key ([string]$ch); Start-Sleep -Milliseconds 45 } }

Write-Host "=== File Explorer address-bar E2E ===" -ForegroundColor Cyan
$args=@("-drive","format=raw,file=$Img","-m","$Mem","-vga","std","-display","none",
        "-machine","pc,mem-merge=off","-accel","tcg","-no-reboot",
        "-serial","file:$(Join-Path $OutDir 'serial.log')",
        "-chardev","socket,id=qmp0,server=on,wait=off,host=127.0.0.1,port=$QmpPort",
        "-mon","chardev=qmp0,mode=control")
$p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $OutDir "qemu.err")
$ready=$false
for($i=0;$i -lt ($BootWait+15);$i++){ Start-Sleep -Seconds 1
    try{ $t=New-Object System.Net.Sockets.TcpClient; $t.Connect("127.0.0.1",$QmpPort); $t.Close(); $ready=$true; break }catch{}
}
if(-not $ready){ Write-Host "[ERROR] QMP not up" -ForegroundColor Red; if(-not $p.HasExited){$p.Kill()}; exit 1 }
Qmp-Connect -Port $QmpPort | Out-Null
Write-Host "booted, waiting 12s for lock screen..." -ForegroundColor Yellow
Start-Sleep -Seconds 12
Qmp-Shot "f01_lock" | Out-Null

# ---- sign in: user field -> clear -> type nexos -> password -> type -> Enter ----
Write-Host "[login] user/pass/key" -ForegroundColor Yellow
Qmp-MoveAbs 640 373; Start-Sleep -Milliseconds 60; Qmp-Click; Start-Sleep -Milliseconds 200
for($k=0;$k -lt 4;$k++){ Qmp-Key "backspace"; Start-Sleep -Milliseconds 60 }
Qmp-Type "nexos"; Start-Sleep -Milliseconds 150
Qmp-MoveAbs 640 431; Start-Sleep -Milliseconds 60; Qmp-Click; Start-Sleep -Milliseconds 200
Qmp-Type "nexos"; Start-Sleep -Milliseconds 150
Qmp-Key "ret"; Start-Sleep -Seconds 2
Qmp-Shot "f02_desktop" | Out-Null

# ---- open File Explorer (taskbar pin 0) ----
Write-Host "[open] File Explorer" -ForegroundColor Yellow
Qmp-MoveAbs 525 696; Start-Sleep -Milliseconds 80; Qmp-Click; Start-Sleep -Seconds 1
Qmp-Shot "f03_explorer" | Out-Null

# ---- focus address bar and type an INVALID address ----
Write-Host "[addr] invalid address" -ForegroundColor Yellow
Qmp-MoveAbs 715 281; Start-Sleep -Milliseconds 80; Qmp-Click; Start-Sleep -Milliseconds 300
Qmp-Shot "f04_focus" | Out-Null
Qmp-Type "zzz"; Start-Sleep -Milliseconds 150
Qmp-Key "ret"; Start-Sleep -Milliseconds 700
Qmp-Shot "f05_invalid" | Out-Null

# ---- dismiss dialog, navigate to a VALID address (S:\) ----
Write-Host "[addr] dismiss + valid address" -ForegroundColor Yellow
Qmp-MoveAbs 700 450; Start-Sleep -Milliseconds 80; Qmp-Click; Start-Sleep -Milliseconds 400
Qmp-Shot "f06_dismissed" | Out-Null
Qmp-MoveAbs 715 281; Start-Sleep -Milliseconds 80; Qmp-Click; Start-Sleep -Milliseconds 300
Qmp-Type "s"; Start-Sleep -Milliseconds 150
Qmp-Key "ret"; Start-Sleep -Milliseconds 700
Qmp-Shot "f07_sfs" | Out-Null

# ---- Phase 2: file-list scrolling (many files on SFS) ----
# The explorer now draws EVERY file, so the list overflows and the native
# scrollbar appears on the right edge; drag it, or use the wheel.
Write-Host "[scroll] list with scrollbar" -ForegroundColor Yellow
Start-Sleep -Milliseconds 400
Qmp-Shot "f08_list" | Out-Null

# Drag the vertical scrollbar thumb down (FE window: client x 381..899, y 254..582;
# scrollbar strip at x ~889..899).  Press near the top, drag toward the bottom.
Write-Host "[scroll] drag scrollbar down" -ForegroundColor Yellow
Qmp-MoveAbs 894 262; Start-Sleep -Milliseconds 80
Qmp-Btn $true
for ($s = 1; $s -le 10; $s++) { Qmp-MoveAbs 894 (262 + $s * 28); Start-Sleep -Milliseconds 30 }
Qmp-Btn $false
Start-Sleep -Milliseconds 400
Qmp-Shot "f09_dragged" | Out-Null

# Mouse wheel over the list (wheel-down a few notches).
Write-Host "[scroll] mouse wheel down" -ForegroundColor Yellow
Qmp-MoveAbs 640 400; Start-Sleep -Milliseconds 60
for ($i = 0; $i -lt 4; $i++) {
    [void](Qmp-Send '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"wheel-down","down":true}}]}}')
    [void](Qmp-Send '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"wheel-down","down":false}}]}}')
    Start-Sleep -Milliseconds 70
}
Start-Sleep -Milliseconds 400
Qmp-Shot "f10_wheel" | Out-Null

[void](Qmp-Send '{"execute":"quit"}'); Start-Sleep -Seconds 2
if(-not $p.HasExited){ $p.Kill() }; $p.WaitForExit(5000)|Out-Null
Write-Host "[DONE] $OutDir" -ForegroundColor Green
Get-ChildItem $OutDir -Filter *.png | ForEach-Object { Write-Host "  $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB" }
