$VBOXMANAGE = "E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$VMNAME = "NexOS"
$ser = 'D:\MyOS\bootloader\build\vbox_serial.log'
$log = 'C:\Users\trans\VirtualBox VMs\NexOS\Logs\VBox.log'

# Clean serial, snapshot log size.
'' | Set-Content $ser
$preSize = (Get-Item $log).Length

# Ensure VM is fully poweroff.
& $VBOXMANAGE controlvm $VMNAME poweroff 2>$null
Start-Sleep -Seconds 2
$st = (& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^VMState=')
Write-Host "State before start: $st"

# Start via VBoxManage (gui) - in this agent env no display, but VM should still RUN.
# Then we read whether SeaBIOS executed our MBR (no VMBootFail) and kernel printed serial.
& $VBOXMANAGE startvm $VMNAME --type gui 2>$null
Start-Sleep -Seconds 12

Write-Host "=== State after ==="
& $VBOXMANAGE showvminfo $VMNAME --machinereadable | Select-String -Pattern '^VMState='
Write-Host "=== Running VMs ==="
& $VBOXMANAGE list runningvms

Write-Host "`n=== Fresh VBox.log (this session) ==="
$cur = Get-Content $log
$cur | Select-Object -Skip $preSize |
  Select-String -Pattern 'Guest Log|Boot|VMBootFail|firmware|EDD' |
  Select-Object -First 50 Line

Write-Host "`n=== Serial log (this session) ==="
Get-Content $ser -Tail 30