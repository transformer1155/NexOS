$VDIDIR = 'D:\MyOS\bootloader\build'
$IMG = Join-Path $VDIDIR 'os_v2.img'

Write-Host "=== os_v2.img size ==="
(Get-Item $IMG).Length

Write-Host "=== VDIs newest first ==="
Get-ChildItem $VDIDIR -Filter '*.vdi' | Sort-Object LastWriteTime -Descending |
  Select-Object Name, Length, LastWriteTime -First 6

$NEWEST = (Get-ChildItem $VDIDIR -Filter 'nexos_vbox_*.vdi' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Host "`n=== Hex of NEWEST VDI (LBA0 + sig) ==="
$bytes = [System.IO.File]::ReadAllBytes($NEWEST)
Write-Host ("Size: {0}" -f $bytes.Length)
Write-Host ("First 32: " + (($bytes[0..31] | ForEach-Object {'{0:X2}' -f $_}) -join ' '))
Write-Host ("Sig 510-511: {0:X2} {1:X2}" -f $bytes[510], $bytes[511])

Write-Host "`n=== Current VBox.log boot-related lines ==="
$log = 'C:\Users\trans\VirtualBox VMs\NexOS\Logs\VBox.log'
Write-Host ("Log size: " + (Get-Item $log).Length)
Select-String -Path $log -Pattern 'Guest Log|Boot|IPL|No boot|PXE|MBR|EDD|INT 13|bootseq|hard disk|Hard Disk' |
  Select-Object -First 40 Line
Write-Host "`n=== Current VBox.log first/last 5 lines ==="
Get-Content $log -TotalCount 5
Write-Host "..."
Get-Content $log -Tail 5