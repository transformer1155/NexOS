# run_nexos_vbox.ps1 - Launch NexOS under VirtualBox.
# Absolute mouse comes from VirtualBox's VMMDev PCI device (no Guest Additions);
# the OS reads absolute coords there, so the host cursor == click position.
$ErrorActionPreference = "Stop"

$VBOXDIR    = "E:\Program Files\Oracle\VirtualBox"
$VBOXMANAGE = Join-Path $VBOXDIR "VBoxManage.exe"
$VBOXVM     = Join-Path $VBOXDIR "VirtualBoxVM.exe"
$VMNAME     = "NexOS"
$IMG        = Join-Path $PSScriptRoot "build\os_v2.img"
$VDI        = Join-Path $PSScriptRoot ("build\nexos_vbox_" + (Get-Date -Format 'yyyyMMdd_HHmmss') + ".vdi")
$SERIALLOG  = Join-Path $PSScriptRoot "build\vbox_serial.log"

if (-not (Test-Path $VBOXMANAGE)) { Write-Error "VBoxManage not found: $VBOXMANAGE"; exit 1 }
if (-not (Test-Path $IMG))        { Write-Error "Image not found: $IMG (build it first)"; exit 1 }

# Release anything holding the raw image (e.g. a lingering headless QEMU that
# mounts os_v2.img read/write) so the conversion below can open it.
function Release-ImageLock {
    $holders = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*os_v2.img*" }
    foreach ($p in $holders) {
        Write-Host "[VBox] Releasing lock: stopping PID $($p.ProcessId) ($($p.Name))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    $running = & $VBOXMANAGE list runningvms
    if ($running -like "*$VMNAME*") {
        Write-Host "[VBox] Powering off running '$VMNAME' VM ..."
        & $VBOXMANAGE controlvm $VMNAME poweroff | Out-Null
    }
    if ($holders) { Start-Sleep -Seconds 1 }
}

Release-ImageLock

# Rebuild the VDI from the latest raw image on every launch, retrying if the
# file is briefly locked.
Write-Host "[VBox] Converting os_v2.img -> $(Split-Path $VDI -Leaf) ..."
$converted = $false
for ($i = 0; $i -lt 5; $i++) {
    & $VBOXMANAGE convertfromraw "$IMG" "$VDI" --format VDI 2>$null | Out-Null
    if (Test-Path $VDI) { $converted = $true; break }
    Release-ImageLock
    Start-Sleep -Seconds 1
}
if (-not $converted) { Write-Error "Failed to create VDI from $IMG (file locked?)"; exit 1 }

# Best-effort cleanup of older VDIs (safe-delete hooks may block deletion; ignore).
Get-ChildItem (Join-Path $PSScriptRoot "build") -Filter "nexos_vbox_*.vdi" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ne $VDI } |
    ForEach-Object { try { Remove-Item $_.FullName -ErrorAction SilentlyContinue } catch {} }

# Create the VM once.
$existing = & $VBOXMANAGE list vms
if ($existing -notlike "*$VMNAME*") {
    Write-Host "[VBox] Creating VM '$VMNAME' ..."
    & $VBOXMANAGE createvm --name $VMNAME --ostype Other --register | Out-Null
    & $VBOXMANAGE modifyvm $VMNAME --memory 2048 --vram 128 `
        --graphicscontroller VBoxVGA --mouse ps2 --keyboard ps2 `
        --firmware bios `
        --boot1 disk --boot2 none --boot3 none --boot4 none `
        --uart1 0x3F8 4 --uartmode1 file "$SERIALLOG" | Out-Null
    & $VBOXMANAGE storagectl $VMNAME --name "IDE" --add ide | Out-Null
}

# Ensure the VM is fully powered off before swapping disks, otherwise
# storageattach silently fails and the VM keeps its old (e.g. NTFS) disk.
& $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
Start-Sleep -Seconds 1
$st = & $VBOXMANAGE showvminfo $VMNAME --machinereadable 2>$null | Select-String -Pattern '^VMState='
if ($st -and $st -notlike '*poweroff*') {
    Write-Host "[VBox] Forcing poweroff..."; & $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
    Start-Sleep -Seconds 2
}

# (Re)attach the disk.
& $VBOXMANAGE storageattach $VMNAME --storagectl "IDE" --port 0 --device 0 `
    --type hdd --medium "$VDI" --nonrotational on | Out-Null

Write-Host "[VBox] Starting '$VMNAME' (GUI). Using relative PS/2 mouse -> OS soft cursor, clicks land on cursor."
Write-Host "       Serial debug log (VMMDev detection): $SERIALLOG"
& $VBOXVM --startvm $VMNAME --type gui
