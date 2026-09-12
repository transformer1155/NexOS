$VBOXMANAGE = "E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$VBOXVM     = "E:\Program Files\Oracle\VirtualBox\VirtualBoxVM.exe"
$VMNAME     = "NexOS"
$IMG        = "D:\MyOS\bootloader\build\os_v2.img"
$VDI        = "D:\MyOS\bootloader\build\nexos_verify_$((Get-Date -Format 'yyyyMMdd_HHmmss')).vdi"

# Make sure VM is off.
& $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
Start-Sleep -Seconds 2

# Convert + attach the CURRENT os_v2.img.
& $VBOXMANAGE convertfromraw $IMG $VDI --format VDI 2>$null | Out-Null
if (-not (Test-Path $VDI)) { Write-Error "convert failed"; exit 1 }
& $VBOXMANAGE storageattach $VMNAME --storagectl "IDE" --port 0 --device 0 `
    --type hdd --medium $VDI --nonrotational on | Out-Null

Write-Host "=== attached medium ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^Attached'

# Snapshot VBox.log size before start.
$log = 'C:\Users\trans\VirtualBox VMs\NexOS\Logs\VBox.log'
$preSize = (Get-Item $log).Length

# Truncate serial log so we read only THIS session.
$ser = 'D:\MyOS\bootloader\build\vbox_serial.log'
'' | Set-Content $ser

# Start headless (detached) and wait for boot.
$proc = Start-Process -FilePath $VBOXVM -ArgumentList "--startvm", $VMNAME, "--type", "headless" -PassThru
Start-Sleep -Seconds 12

Write-Host "=== VMState ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^VMState='
Write-Host "=== Running VMs ==="
& $VBOXMANAGE list runningvms

Write-Host "`n=== Fresh VBox.log boot window (this session only) ==="
Get-Content $log | Select-Object -Skip $preSize |
  Select-String -Pattern 'Guest Log|Boot|VMBootFail|firmware' |
  Select-Object -First 40 Line

Write-Host "`n=== Serial log (THIS session) ==="
Get-Content $ser -Tail 25