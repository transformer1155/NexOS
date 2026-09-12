$ErrorActionPreference = "Continue"
$repo = "d:\MyOS\bootloader"
Set-Location $repo

# NOTE: the bootable image is produced by stitch.ps1 (SFS at LBA 3664, which
# must match kernel.cpp SFS_ALT_LBA and the Makefile SFS_LBA).  Do NOT rebuild
# or truncate it here -- an earlier version of this script rewrote the image at
# LBA 3488 and SetLength(4MB), which desynced from the kernel and made the SFS
# unmountable.  Just boot the already-stitched image.
$dst = "d:\MyOS\bootloader\_rt\boot_test.img"
if (!(Test-Path $dst)) {
    Write-Host "ERROR: $dst not found -- run stitch.ps1 first"
    exit 1
}

# clean stray qemu
Get-Process qemu* -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$log = "_rt/serial.log"
if (Test-Path $log) { Remove-Item $log -Force }

$q = Start-Process -FilePath "D:\qemu\qemu-system-i386.exe" -ArgumentList @(
    "-machine","pc","-m","256",
    "-drive","format=raw,file=$dst",
    "-serial","file:$log",
    "-display","none","-accel","tcg","-no-reboot"
) -PassThru
Start-Sleep -Seconds 45
if (!$q.HasExited) { $q.Kill(); Start-Sleep -Seconds 1 }
Get-Process qemu* -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "=== serial.log ==="
if (Test-Path $log) {
    $raw = [System.IO.File]::ReadAllBytes($log)
    $s = -join ($raw | ForEach-Object { if($_ -ge 32 -and $_ -lt 127){[char]$_} elseif($_ -eq 10){"`n"} elseif($_ -eq 13){""} else{"."} })
    Write-Host $s
} else { Write-Host "(no serial log)" }
