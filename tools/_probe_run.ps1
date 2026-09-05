$q = Start-Process -FilePath 'D:\qemu\qemu-system-x86_64.exe' -ArgumentList @(
  '-drive', 'file=\MyOS\bootloader\build\os_v2.img,format=raw',
  '-chardev', 'socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=4321',
  '-serial', 'chardev:ser0',
  '-display', 'none', '-vga', 'std', '-machine', 'pc,mem-merge=off',
  '-m', '512', '-accel', 'tcg', '-no-reboot'
) -PassThru -NoNewWindow
Write-Host ("QEMU_PID=" + $q.Id)
Start-Sleep -Seconds 3
$b = Start-Process -FilePath 'python3' -ArgumentList 'D:\MyOS\Bootloader\tools\nexos_bridge.py' -PassThru -NoNewWindow
Write-Host ("BRIDGE_PID=" + $b.Id)
Start-Sleep -Seconds 2
python3 'D:\MyOS\Bootloader\tools\_probe.py'
Stop-Process -Id $q.Id, $b.Id -Force -ErrorAction SilentlyContinue
