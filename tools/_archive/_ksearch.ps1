Select-String -Path 'D:\MyOS\Bootloader\kernel.cpp' -Pattern 'serial|Serial|COM1|uart|UART|0x3F8|read_serial|write_serial' | ForEach-Object { Write-Host ($_.LineNumber.ToString() + ': ' + $_.Line) }
