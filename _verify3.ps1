$VBOXMANAGE = "E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
Start-Sleep -Seconds 10
$ser = 'D:\MyOS\bootloader\build\vbox_serial.log'
$log = 'C:\Users\trans\VirtualBox VMs\NexOS\Logs\VBox.log'

Write-Host "=== Serial log (all, byte length) ==="
(Get-Item $ser).Length
Get-Content $ser -Tail 40

Write-Host "`n=== Fresh VBox.log (last 60 lines of whole log) ==="
Get-Content $log -Tail 60

Write-Host "`n=== VMState ==="
& $VBOXMANAGE showvminfo NexOS --machinereadable | Select-String -Pattern '^VMState='