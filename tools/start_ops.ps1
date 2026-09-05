<#
  start_ops.ps1 - systemd-style supervisor for the NexOS ops panel.

  Launches QEMU in ops-bridge mode (serial -> tcp:127.0.0.1:4321) and the
  host WebSocket bridge, then keeps both alive. Ctrl+C stops everything.

  Usage:
    powershell -ExecutionPolicy Bypass -File bootloader/tools/start_ops.ps1
    bootloader/tools/start_ops.ps1 -NoBridge      # QEMU only
    bootloader/tools/start_ops.ps1 -Foreground    # run QEMU in this window
#>
param(
    [switch]$NoBridge,
    [switch]$Foreground
)

$ErrorActionPreference = "Stop"
$qemu = "D:\qemu\qemu-system-x86_64.exe"
$py   = "python"

if (-not (Test-Path $qemu)) { Write-Host "missing QEMU: $qemu" -ForegroundColor Red; exit 1 }

# ---- Locate the boot image (search a few known build dirs) ----
$toolsDir = $PSScriptRoot
$candidates = @(
    (Join-Path $toolsDir "..\build\os_uefi.img"),
    (Join-Path $toolsDir "..\build\os.img"),
    (Join-Path $toolsDir "..\build\os_v2.img"),
    "D:\MyOS\win11-desktop\bootloader\build\os_uefi.img",
    "D:\MyOS\bootloader\build\os_v2.img"
)
$img = $null
foreach ($c in $candidates) {
    $p = [System.IO.Path]::GetFullPath($c)
    if (Test-Path $p) { $img = $p; break }
}
if (-not $img) { Write-Host "missing image (looked for os_uefi.img / os.img / os_v2.img under build dirs)" -ForegroundColor Red; exit 1 }
Write-Host "Using image: $img" -ForegroundColor Cyan

$jobs = @()

# ---- QEMU (ops bridge mode: serial over TCP) ----
$qemuArgs = @("-m","512","-vga","std","-display","sdl","-no-reboot",
              "-net","nic,model=ne2k_isa","-net","user",
              "-chardev","socket,id=ser,host=127.0.0.1,port=4321,server=on,wait=off",
              "-serial","chardev:ser",
              "-drive","format=raw,file=$img")

if ($Foreground) {
    Write-Host "Launching QEMU in foreground..." -ForegroundColor Cyan
    & $qemu @qemuArgs
} else {
    $q = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
    $jobs += $q
    Write-Host "QEMU started (pid $($q.Id)) - serial tcp:127.0.0.1:4321" -ForegroundColor Green
}

# ---- Host bridge (WebSocket <-> serial TCP) ----
if (-not $NoBridge) {
    $bridge = Join-Path $toolsDir "nexos_bridge.py"
    $b = Start-Process -FilePath $py -ArgumentList @($bridge) -PassThru
    $jobs += $b
    Write-Host "Bridge started (pid $($b.Id)) - ws://localhost:8765" -ForegroundColor Green
}

Write-Host "`nOps stack running. Press Ctrl+C to stop all." -ForegroundColor Yellow
try {
    while ($true) {
        foreach ($j in $jobs) {
            if ($j.HasExited -and -not $j.Tag) {
                Write-Host "Process $($j.Id) exited unexpectedly." -ForegroundColor Red
                $j.Tag = $true
            }
        }
        Start-Sleep -Seconds 2
    }
} finally {
    Write-Host "`nStopping ops stack..." -ForegroundColor Yellow
    foreach ($j in $jobs) {
        if (-not $j.HasExited) { Stop-Process -Id $j.Id -Force -ErrorAction SilentlyContinue }
    }
    Write-Host "Done." -ForegroundColor Green
}
