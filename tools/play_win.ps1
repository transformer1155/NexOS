# Pure-Windows NexOS GUI test launcher (NO WSL).
# Opens a real QEMU window so the desktop can be driven by hand.
#
# Default image is build\os.img (auto-GUI build: boots straight into the
# Win11 desktop with the graphical lock screen).  Use -Img build\os_textboot.img
# for the text-boot variant (console login, GUI only on demand).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools/play_win.ps1 [-Img build\os.img]
param(
    [string]$Img = "build\os.img"
)

$ErrorActionPreference = "Stop"
$qemu  = "D:\qemu\qemu-system-x86_64.exe"
$root  = Split-Path -Parent $PSScriptRoot
$drive = Join-Path $root $Img
$log   = Join-Path $root "build\serial_win_gui.log"

if (-not (Test-Path $qemu))  { Write-Host "missing QEMU: $qemu" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $drive)) { Write-Host "missing image: $drive  (run tools/build_win.sh textboot first)" -ForegroundColor Red; exit 1 }

New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
if (Test-Path $log) { Remove-Item $log -Force }

Write-Host "Starting NexOS in a QEMU window..." -ForegroundColor Cyan
Write-Host "  image : $drive"
Write-Host "  serial: $log"

Start-Process -FilePath $qemu -ArgumentList @(
    "-m", "4096",
    "-vga", "std",
    "-display", "sdl",
    "-no-reboot",
    "-net", "nic,model=ne2k_isa",
    "-net", "user",
    "-chardev", "file,id=ser,path=$log",
    "-serial", "chardev:ser",
    "-drive", "format=raw,file=$drive"
)
Write-Host "Window launched. Serial console is being logged to: $log" -ForegroundColor Green
