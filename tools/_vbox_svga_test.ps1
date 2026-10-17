# Headless verification of the NexOS VMware SVGA-II driver under VirtualBox.
# Boots the BIOS image with the VMSVGA controller (exposes the VMware
# 15AD:0405 SVGA-II device), captures the serial console, and takes a
# screenshot so we can confirm the display is not striped/blanked by
# ENABLE and that the hardware cursor came up.
$ErrorActionPreference = "Continue"

# Graphics controller to expose.  In this VirtualBox build the "vboxsvga"
# controller still enumerates the PCI VGA device as 80EE:BEEF (VBoxVGA), so
# the VMware SVGA-II driver (which binds to 15AD:0405) never sees a device.
# "vmsvga" is the one that actually exposes 15AD:0405.  Pass a controller
# name as the first argument to override.
$Controller  = if ($args.Count -gt 0) { $args[0] } else { "vmsvga" }

$VBOX       = "C:\Program Files\Oracle\VBoxManage.exe"
$ROOT       = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VMNAME     = "NexOS_SVGA_Test"
$IMG        = Join-Path $ROOT "build\os_v2.img"
$SERIALLOG  = Join-Path $ROOT "build\vbox_svga_serial.log"
$SCREENSHOT = Join-Path $ROOT "build\vbox_svga_screen.png"

if (-not (Test-Path $VBOX))      { Write-Error "VBoxManage not found: $VBOX"; exit 1 }
if (-not (Test-Path $IMG))       { Write-Error "Image not found: $IMG"; exit 1 }

# Kill any stale VM processes holding locks.
Get-Process -EA SilentlyContinue -Name "VirtualBoxVM","VBoxHeadless","VBoxSVC" |
    ForEach-Object { Write-Host "[t] killing $($_.Name) PID $($_.Id)"; Stop-Process -Id $_.Id -Force -EA SilentlyContinue }
Start-Sleep -Seconds 1

# Clean up a previous test VM if it exists.
$prev = & $VBOX list vms 2>$null
if ($prev -like "*$VMNAME*") {
    & $VBOX controlvm $VMNAME poweroff 2>$null
    Start-Sleep -Seconds 1
    & $VBOX unregistervm $VMNAME --delete 2>$null
    Start-Sleep -Seconds 1
}

# Fresh VDI from the flat image.
# VERR_SHARING_VIOLATION is common right after a WSL `make`: the WSL 9p/host
# side can still hold the image open briefly.  Retry a few times after asking
# WSL to shut down so the handle is released.
$VDI = Join-Path $ROOT ("build\nexos_svga_" + (Get-Date -Format 'yyyyMMdd_HHmmss') + ".vdi")
for ($try = 1; $try -le 4; $try++) {
    & $VBOX convertfromraw "$IMG" "$VDI" --format VDI 2>$null | Out-Null
    if (Test-Path $VDI) { break }
    Write-Host "[t] convertfromraw attempt $try failed; releasing WSL handles and retrying..."
    & wsl.exe --shutdown 2>$null | Out-Null
    Start-Sleep -Seconds (3 * $try)
}
if (-not (Test-Path $VDI)) { Write-Error "convertfromraw failed"; exit 1 }
Write-Host "[t] VDI: $(Split-Path $VDI -Leaf)"

# Create + configure the test VM with the VMware SVGA-II controller.
# IMPORTANT: must be `vmsvga` (or `vboxsvga`), NOT `vboxvga`.  `vboxvga`
# exposes the VBox VGA device (80EE:BEEF) which the SVGA-II driver does not
# bind to, so it silently no-ops.  In this VirtualBox build `vboxsvga` ALSO
# enumerates as 80EE:BEEF, so we default to `vmsvga`, which exposes the
# VMware SVGA-II device (15AD:0405) the driver targets, while VBE
# (0x500D=1) and the LFB stay active.
& $VBOX createvm --name $VMNAME --ostype Other --register 2>$null | Out-Null
& $VBOX modifyvm $VMNAME --memory 2048 --vram 128 --cpus 1 `
    --graphicscontroller $Controller --mouse usbtablet --keyboard ps2 `
    --nic1 none `
    --uart1 0x3F8 4 --uartmode1 file "$SERIALLOG" 2>$null | Out-Null
Write-Host "[t] graphics controller: $Controller (nic1 disabled so TZ auto-detect does not block boot)"
& $VBOX storagectl $VMNAME --name "IDE" --add ide 2>$null | Out-Null
& $VBOX storageattach $VMNAME --storagectl "IDE" --port 0 --device 0 `
    --type hdd --medium "$VDI" --nonrotational on 2>$null | Out-Null

# Clear old serial log.
if (Test-Path $SERIALLOG) { Remove-Item $SERIALLOG -Force -EA SilentlyContinue }
Start-Sleep -Seconds 1

Write-Host "[t] Starting headless..."
& $VBOX startvm $VMNAME --type headless 2>$null | Out-Null
Start-Sleep -Seconds 15
& $VBOX controlvm $VMNAME screenshotpng "$ROOT\build\bt_15.png" 2>$null | Out-Null
Start-Sleep -Seconds 15
& $VBOX controlvm $VMNAME screenshotpng "$ROOT\build\bt_30.png" 2>$null | Out-Null

# Inject a mouse event so any lazy VMMDev / cursor probe fires.
& $VBOX controlvm $VMNAME mouseput 400 300 2>$null | Out-Null
Start-Sleep -Seconds 6

# Grab a screenshot to confirm the display is alive and not striped.
& $VBOX controlvm $VMNAME screenshotpng "$SCREENSHOT" 2>$null | Out-Null
if (Test-Path $SCREENSHOT) { Write-Host "[t] screenshot: $SCREENSHOT" } else { Write-Host "[t] (no screenshot produced)" }

# Power off FIRST so VBox releases the serial log file handle, then read it.
& $VBOX controlvm $VMNAME poweroff 2>$null | Out-Null
$waited = 0
while ($waited -lt 20) {
    $st = (& $VBOX showvminfo $VMNAME --machinereadable 2>$null | Select-String "VMState=")
    if ($st -match "VMState=""poweroff""") { break }
    Start-Sleep -Seconds 1; $waited++
}

Write-Host "=== SVGA DRIVER MARKERS ==="
if (Test-Path $SERIALLOG) {
    $txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($SERIALLOG))
    ($txt -split "`n") | Where-Object { $_ -match "SVGA|CURSOR|scanout|ENABL|format|fb=|readback|device found|15AD" } | ForEach-Object { $_ }
} else { Write-Host "(no serial log produced)" }

Write-Host "=== SERIAL LOG (tail 50) ==="
if (Test-Path $SERIALLOG) {
    $txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($SERIALLOG))
    ($txt -split "`n") | Select-Object -Last 50 | ForEach-Object { $_ }
} else { Write-Host "(no serial log)" }

& $VBOX unregistervm $VMNAME --delete 2>$null | Out-Null
Get-ChildItem (Join-Path $ROOT "build") -Filter "nexos_svga_*.vdi" -EA SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -EA SilentlyContinue } catch {} }
Write-Host "[t] done"

