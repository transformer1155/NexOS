# qemu_win.ps1 - launch NexOS in Windows QEMU and drive it over the serial port.
$ErrorActionPreference = "SilentlyContinue"
$q = "D:\Program Files\qemu\qemu-system-x86_64.exe"
$img = "E:\NexOS\NexOS\build\os_v2.img"
$outdir = "E:\NexOS\NexOS"
$log = Join-Path $outdir "_qemu_serial.log"
$shot = Join-Path $outdir "_qemu_shot.ppm"
$ser_port = 1234
$mon_port = 1235

# 1) launch QEMU (visible window so the user can watch; reduce vCPUs for stability)
$args = @(
  "-drive", "format=raw,file=$img",
  "-m", "256",
  "-accel", "tcg",
  "-vga", "std",
  "-display", "gtk",
  "-monitor", "tcp:127.0.0.1:$mon_port,server,nowait",
  "-serial", "tcp:127.0.0.1:$ser_port,server,nowait",
  "-no-reboot"
)
Start-Process -FilePath $q -ArgumentList $args -WindowStyle Normal
Write-Host "QEMU launched"

# 2) connect to the serial TCP server
$client = $null
for ($i=0; $i -lt 40; $i++) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $ser_port)
        if ($client.Connected) { Write-Host "SERIAL connected"; break }
    } catch { Start-Sleep -Seconds 1 }
}
if (-not $client -or -not $client.Connected) { Write-Host "SERIAL FAIL"; exit 1 }

$stream = $client.GetStream()
$sw = New-Object System.IO.StreamWriter($stream); $sw.AutoFlush = $true
$sr = New-Object System.IO.StreamReader($stream)

function Send-GT($cmd) {
    $sw.WriteLine($cmd)
    Start-Sleep -Seconds 2
    $lines = @(); $stream.ReadTimeout = 2500
    try { while ($true) { $l = $sr.ReadLine(); if ($null -eq $l) { break }; $lines += $l } } catch {}
    $lines
}

# wait for OS to reach GUI
Start-Sleep -Seconds 18

"===== gt status #1 =====" | Out-File -Append -Encoding ascii $log
Send-GT "gt status" | Out-File -Append -Encoding ascii $log

# 3) screenshot via QEMU monitor
function Shot($path) {
    $m = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $mon_port)
    $ms = $m.GetStream(); $mw = New-Object System.IO.StreamWriter($ms); $mw.AutoFlush = $true
    $mw.WriteLine("screendump $path")
    Start-Sleep -Seconds 1
    $mw.WriteLine("quit"); $m.Close()
}
Shot $shot
Write-Host "Screenshot -> $shot"

# isolation test: send a second status 10s later to see if the busy loop is alive
Start-Sleep -Seconds 10
"===== gt status #2 (10s later) =====" | Out-File -Append -Encoding ascii $log
Send-GT "gt status" | Out-File -Append -Encoding ascii $log
Shot (Join-Path $outdir "_qemu_shot_fe.ppm")
Write-Host "Screenshot (FE) -> $(Join-Path $outdir '_qemu_shot_fe.ppm')"

"===== DONE =====" | Out-File -Append -Encoding ascii $log
Write-Host "Log -> $log"
$client.Close()
