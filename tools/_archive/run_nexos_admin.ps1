<#
.SYNOPSIS
    Launch NexOS under QEMU with a UAC elevation prompt and a large RAM size.

.DESCRIPTION
    NexOS caps its physical page allocator at 1 GiB (PMM_MAX_PAGES in
    kernel.cpp).  Giving QEMU more than that is pointless, but it *does* need
    enough headroom for the built-in VMM to carve out a 256 MiB guest RAM
    region plus the Linux heap.

    Allocating large guest memory can require elevated privileges on some
    Windows configurations (locked pages / Hyper-V reservation), and QEMU may
    need to open devices that a standard user token cannot.  This script
    therefore re-launches itself elevated via UAC before starting QEMU.

.PARAMETER MemMB
    RAM to give the NexOS VM in MiB.  Default 4096 (4 GiB).  Raise to 8192 if
    your host has enough free RAM and you want the largest possible guest.

.PARAMETER Display
    QEMU display backend: sdl, gtk, or none.  Default: sdl.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\run_nexos_admin.ps1
    powershell -ExecutionPolicy Bypass -File tools\run_nexos_admin.ps1 -MemMB 8192
#>
[CmdletBinding()]
param(
    [int]$MemMB = 4096,
    [string]$Display = "sdl",
    [string]$Image = "",
    [switch]$NoElevate
)

$ErrorActionPreference = "Stop"

# ---- Re-launch elevated (UAC) ----------------------------------------
function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    if ($NoElevate) {
        Write-Host "[nexos] WARNING: not elevated; continuing anyway (-NoElevate)." -ForegroundColor Yellow
    } else {
        Write-Host "[nexos] Requesting administrator privileges (UAC)..." -ForegroundColor Cyan
        $argList = @(
            "-NoProfile"
            "-ExecutionPolicy", "Bypass"
            "-File", "`"$PSCommandPath`""
            "-MemMB", "$MemMB"
            "-Display", "$Display"
            "-NoElevate"
        )
        if ($Image) { $argList += @("-Image", "`"$Image`"") }
        try {
            Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs
        } catch {
            Write-Host "[nexos] Elevation was declined or failed: $_" -ForegroundColor Red
            Write-Host "[nexos] Re-run with -NoElevate to start without admin rights." -ForegroundColor Yellow
        }
        exit
    }
} else {
    Write-Host "[nexos] Running elevated." -ForegroundColor Green
}

# ---- Locate the repo and image ---------------------------------------
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Image) {
    $Image = Join-Path $repo "build\os_v2.img"
}
if (-not (Test-Path $Image)) {
    Write-Host "[nexos] Image not found: $Image" -ForegroundColor Red
    Write-Host "[nexos] Build it first:  wsl make" -ForegroundColor Yellow
    exit 1
}

# ---- Find QEMU --------------------------------------------------------
$qemu = $null
foreach ($c in @("qemu-system-x86_64.exe", "qemu-system-i386.exe")) {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { $qemu = $cmd.Source; break }
}
if (-not $qemu) {
    $candidates = @(
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $qemu = $c; break }
    }
}
if (-not $qemu) {
    Write-Host "[nexos] QEMU not found. Install it or add qemu-system-x86_64 to PATH." -ForegroundColor Red
    exit 1
}

# ---- Report the memory budget ----------------------------------------
$pmmCapMiB = 1024   # must match PMM_MAX_PAGES in kernel.cpp
Write-Host ""
Write-Host "=== NexOS ===" -ForegroundColor Cyan
Write-Host ("  QEMU      : {0}" -f $qemu)
Write-Host ("  Image     : {0}" -f $Image)
Write-Host ("  VM RAM    : {0} MiB" -f $MemMB)
Write-Host ("  PMM cap   : {0} MiB (kernel.cpp PMM_MAX_PAGES)" -f $pmmCapMiB)
Write-Host ("  Guest RAM : up to 256 MiB carved from PMM by vmm.cpp")
Write-Host ("  Display   : {0}" -f $Display)
if ($MemMB -gt $pmmCapMiB) {
    Write-Host ("  NOTE      : {0} MiB given to QEMU but NexOS only manages {1} MiB." -f $MemMB, $pmmCapMiB) -ForegroundColor Yellow
    Write-Host ("              Raise PMM_MAX_PAGES in kernel.cpp to use more.") -ForegroundColor Yellow
}
Write-Host ""

# ---- Launch -----------------------------------------------------------
$args = @(
    "-drive", "format=raw,file=$Image",
    "-m", "$MemMB",
    "-vga", "std",
    "-display", $Display,
    "-net", "nic,model=ne2k_isa",
    "-net", "user,hostfwd=tcp::8080-:8080"
)
Write-Host ("[nexos] {0} {1}" -f $qemu, ($args -join " ")) -ForegroundColor DarkGray
& $qemu @args
