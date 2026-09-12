$VBOXMANAGE = "E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$VMNAME = "NexOS"

# Kill stray VirtualBoxVM processes.
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -eq 'VirtualBoxVM.exe' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

# Power off the VM and wait until it reports poweroff.
& $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
Start-Sleep -Seconds 2
for ($i=0; $i -lt 10; $i++) {
    $st = (& $VBOXMANAGE showvminfo $VMNAME --machinereadable 2>$null | Select-String -Pattern '^VMState=')
    if ($st -like '*poweroff*') { break }
    Start-Sleep -Seconds 1
}

# Apply the firmware fix (and re-affirm boot order).
& $VBOXMANAGE modifyvm $VMNAME --firmware bios --boot1 disk --boot2 none --boot3 none --boot4 none 2>$null

Write-Host "=== firmware + boot after fix ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^(firmware|boot1|boot2|VMState)='
Write-Host "=== attached medium ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^(ide|medium|sata|nvme)'