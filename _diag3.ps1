$VBOXMANAGE = "E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$log = 'C:\Users\trans\VirtualBox VMs\NexOS\Logs\VBox.log'
$VDI = 'D:\MyOS\bootloader\build\nexos_vbox_20260905_211717.vdi'

Write-Host "=== Boot window (00:00:04..00:00:11) ==="
Select-String -Path $log -Pattern '^00:00:0[4-9]\.|^00:00:1[0-1]\.' |
  ForEach-Object { $_.Line }

Write-Host "`n=== VDI geometry (showmediuminfo) ==="
& $VBOXMANAGE showmediuminfo disk "$VDI" 2>$null |
  Select-Object -Pattern 'Geometry|Size|Capacity|Sector|Logical|Physical|CHS'

Write-Host "`n=== Attached media to NexOS ==="
& $VBOXMANAGE showvminfo "NexOS" --machinereadable |
  Select-String -Pattern '^(storagecontrollername|ide|machineuuid|boot|attached)'