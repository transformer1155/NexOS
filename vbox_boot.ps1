# vbox_boot.ps1 - start NexOS VM in VirtualBox (headful) and capture serial boot log.
$ErrorActionPreference = "SilentlyContinue"
$vm = "C:\Program Files\Oracle\VBoxManage.exe"
$log = "E:\NexOS\NexOS\_vbox_serial.log"
$outdir = "E:\NexOS\NexOS"
$ser_port = 1234

# start VM if not running
$rv = & $vm list runningvms 2>&1
if ($rv -notmatch "NexOS") {
    Write-Host "Starting VM (headful)..."
    & $vm startvm "NexOS" 2>&1 | Out-Host
    Start-Sleep -Seconds 3
} else {
    Write-Host "VM already running"
}

# connect to COM1 tcp server
$client = $null
for ($i=0; $i -lt 30; $i++) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $ser_port)
        if ($client.Connected) { Write-Host "SERIAL connected"; break }
    } catch { Start-Sleep -Seconds 1 }
}
if (-not $client -or -not $client.Connected) { Write-Host "SERIAL FAIL"; exit 1 }

$stream = $client.GetStream()
$sr = New-Object System.IO.StreamReader($stream)
$buf = New-Object System.Text.StringBuilder
$stream.ReadTimeout = 1000
$deadline = [datetime]::Now.AddSeconds(40)
try {
    while ([datetime]::Now -lt $deadline) {
        try {
            $ch = $sr.Read()
            if ($ch -lt 0) { Start-Sleep -Milliseconds 200; continue }
            [void]$buf.Append([char]$ch)
        } catch {
            # timeout: keep looping until deadline
        }
    }
} catch {}
$client.Close()

# sanitize to ASCII and save
$raw = $buf.ToString()
$sane = ""
foreach ($c in $raw.ToCharArray()) {
    $b = [int]$c
    if ($b -ge 0x20 -and $b -lt 0x7F) { $sane += [char]$c }
    elseif ($c -eq "`r" -or $c -eq "`n") { $sane += $c }
    else { $sane += "." }
}
[System.IO.File]::WriteAllText($log, $sane, [System.Text.Encoding]::ASCII)
Write-Host "Log -> $log"
