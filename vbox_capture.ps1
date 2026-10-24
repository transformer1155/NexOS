# vbox_capture.ps1 - drives the NexOS VM from outside via COM1 (TCP) + screenshots.
$ErrorActionPreference = "SilentlyContinue"
$vm = "C:\Program Files\Oracle\VBoxManage.exe"
$name = "NexOS"
$host_ip = "127.0.0.1"
$port = 1234
$outdir = 'E:\NexOS\NexOS'
$log = Join-Path $outdir "_vbox_serial.log"
$shot = Join-Path $outdir "_vbox_shot.png"

# 1) ensure the VM is running (headful so the user can watch/interact)
$r = & $vm list runningvms 2>&1
if (-not ($r -match "NexOS")) {
    Write-Host "Starting VM (headful)..."
    Start-Process -FilePath $vm -ArgumentList "startvm","$name" -WindowStyle Hidden
    Start-Sleep -Seconds 5
} else {
    Write-Host "VM already running"
}

# 2) connect to the COM1 TCP server
$client = $null
for ($i=0; $i -lt 40; $i++) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient($host_ip, $port)
        if ($client.Connected) { Write-Host "TCP connected"; break }
    } catch {
        Start-Sleep -Seconds 1
    }
}
if (-not $client -or -not $client.Connected) { Write-Host "TCP FAIL"; exit 1 }

$stream = $client.GetStream()
$sw = New-Object System.IO.StreamWriter($stream)
$sr = New-Object System.IO.StreamReader($stream)
$sw.AutoFlush = $true

function Send-GT($cmd) {
    $sw.WriteLine($cmd)
    Start-Sleep -Milliseconds 900
    $lines = @()
    $stream.ReadTimeout = 900
    try { while ($true) { $l = $sr.ReadLine(); if ($null -eq $l) { break }; $lines += $l } } catch {}
    $lines
}

# let the OS reach the GUI
Start-Sleep -Seconds 12

"===== gt status (boot) =====" | Out-File $log -Append
Send-GT "gt status" | Out-File $log -Append

# 3) screenshot what is on screen
& $vm controlvm $name screenshotpng $shot 2>&1
Write-Host "Screenshot -> $shot"

"===== gt fe_open =====" | Out-File $log -Append
Send-GT "gt fe_open" | Out-File $log -Append
"===== gt status (after fe_open) =====" | Out-File $log -Append
Send-GT "gt status" | Out-File $log -Append

# screenshot again after opening FE
& $vm controlvm $name screenshotpng (Join-Path $outdir "_vbox_shot_fe.png") 2>&1
Write-Host "Screenshot (FE) saved"

Start-Sleep -Seconds 3
"===== DONE =====" | Out-File $log -Append
Write-Host "Log -> $log"
$client.Close()
