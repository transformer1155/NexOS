Set-Location 'd:\MyOS\bootloader'
Remove-Item 'build\uefi_boot.log' -ErrorAction SilentlyContinue
$qemu = 'D:\qemu\qemu-system-x86_64.exe'
$args = @(
  '-machine','q35','-m','512','-accel','tcg',
  '-drive','if=pflash,format=raw,readonly=on,file=build/ovmf_code.fd',
  '-drive','if=pflash,format=raw,file=build/ovmf_vars.fd',
  '-drive','format=raw,file=build/os_uefi.img',
  '-display','none',
  '-serial','file:build/uefi_boot.log',
  '-no-reboot'
)
$p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 55
if (-not $p.HasExited) { $p.Kill() }
Write-Output 'DONE'
