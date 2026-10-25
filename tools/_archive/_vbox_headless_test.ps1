# Headless verification of NexOS under VirtualBox (no GUI needed).
# Uses a throwaway test VM to avoid clashing with a locked "NexOS" VM.
$ErrorActionPreference = "Continue"
$ROOT       = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VBOXDIR    = "E:\Program Files\Oracle\VirtualBox"
$VBOXMANAGE = Join-Path $VBOXDIR "VBoxManage.exe"
$VMNAME     = "NexOS_Test"
$IMG        = Join-Path $ROOT "build\os_v2.img"
$SERIALLOG  = Join-Path $ROOT "build\vbox_serial.log"

if (-not (Test-Path $VBOXMANAGE)) { Write-Error "VBoxManage not found: $VBOXMANAGE"; exit 1 }
if (-not (Test-Path $IMG))        { Write-Error "Image not found: $IMG"; exit 1 }

# Kill any stale VM processes / VBox service holding locks.
Get-Process -EA SilentlyContinue -Name "VirtualBoxVM","VBoxHeadless" |
    ForEach-Object { Write-Host "[t] killing $($_.Name) PID $($_.Id)"; Stop-Process -Id $_.Id -Force -EA SilentlyContinue }
Start-Sleep -Seconds 1

# Clean up a previous test VM if it exists.
$prev = & $VBOXMANAGE list vms
if ($prev -like "*$VMNAME*") {
    & $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
    Start-Sleep -Seconds 1
    & $VBOXMANAGE unregistervm $VMNAME --delete 2>$null
    Start-Sleep -Seconds 1
}

# Fresh VDI
$VDI = Join-Path $ROOT ("build\nexos_headless_" + (Get-Date -Format 'yyyyMMdd_HHmmss') + ".vdi")
& $VBOXMANAGE convertfromraw "$IMG" "$VDI" --format VDI 2>$null | Out-Null
if (-not (Test-Path $VDI)) { Write-Error "convertfromraw failed"; exit 1 }
Write-Host "[t] VDI: $(Split-Path $VDI -Leaf)"

# Create + configure the test VM
& $VBOXMANAGE createvm --name $VMNAME --ostype Other --register 2>$null | Out-Null
& $VBOXMANAGE modifyvm $VMNAME --memory 2048 --vram 128 `
    --graphicscontroller VBoxVGA --mouse ps2 --keyboard ps2 `
    --uart1 0x3F8 4 --uartmode1 file "$SERIALLOG" 2>$null | Out-Null
& $VBOXMANAGE storagectl $VMNAME --name "IDE" --add ide 2>$null | Out-Null
& $VBOXMANAGE storageattach $VMNAME --storagectl "IDE" --port 0 --device 0 `
    --type hdd --medium "$VDI" --nonrotational on 2>$null | Out-Null

# Clear old serial log
if (Test-Path $SERIALLOG) { Remove-Item $SERIALLOG -Force -EA SilentlyContinue }
Start-Sleep -Seconds 1

Write-Host "[t] Starting headless..."
& $VBOXMANAGE startvm $VMNAME --type headless 2>$null | Out-Null
Start-Sleep -Seconds 12

# The VMMDev probe result is printed by the kernel to the serial log
# (grep below).  Guest-memory introspection (debugvm/dumpvmcore) is not
# reliable in this VirtualBox build, so we rely on the serial output.

# Inject a mouse event so the lazy VMMDev probe (first mouse event) fires.
& $VBOXMANAGE controlvm $VMNAME mouseput 400 300 2>$null | Out-Null
Start-Sleep -Seconds 3

Write-Host "=== VBOX / mouse detection ==="
if (Test-Path $SERIALLOG) {
    Select-String -Path $SERIALLOG -Pattern "VBOX|absolute|mouse|Mouse Integration" -CaseSensitive:$false |
        ForEach-Object { $_.Line }
} else { Write-Host "(no serial log produced)" }
Write-Host "=== SERIAL LOG (tail 40) ==="
if (Test-Path $SERIALLOG) { Get-Content $SERIALLOG -Tail 40 } else { Write-Host "(no serial log)" }
Write-Host "=== VM STATE ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable 2>$null | Select-String "VMState="

& $VBOXMANAGE controlvm $VMNAME poweroff 2>$null | Out-Null
& $VBOXMANAGE unregistervm $VMNAME --delete 2>$null | Out-Null
Get-ChildItem (Join-Path $ROOT "build") -Filter "nexos_headless_*.vdi" -EA SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -EA SilentlyContinue } catch {} }
Write-Host "[t] done"
