$p = Start-Process -FilePath 'D:\qemu\qemu-system-x86_64.exe' -ArgumentList @(
  '-drive', 'file=\MyOS\bootloader\build\os_v2.img,format=raw',
  '-chardev', 'socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=4321',
  '-serial', 'chardev:ser0',
  '-display', 'none', '-vga', 'std', '-machine', 'pc,mem-merge=off',
  '-m', '384', '-accel', 'tcg', '-no-reboot'
) -PassThru -NoNewWindow -RedirectStandardError 'D:\MyOS\Bootloader\tools\_qerr.txt'
Write-Host ('PID=' + $p.Id)
Start-Sleep -Seconds 8
if (-not $p.HasExited) {
  Write-Host 'RUNNING -> serial should be listening'
  Write-Host (Get-Content 'D:\MyOS\Bootloader\tools\_qerr.txt' -Raw)
  Stop-Process -Id $p.Id -Force
} else {
  Write-Host ('EXITED early code=' + $p.ExitCode)
  Write-Host (Get-Content 'D:\MyOS\Bootloader\tools\_qerr.txt' -Raw)
}
