$img = 'D:\MyOS\bootloader\build\os_v2.img'
$bytes = [System.IO.File]::ReadAllBytes($img)
Write-Host "=== Size ==="; $bytes.Length
Write-Host "=== First 64 bytes (MBR head) ==="
($bytes[0..63] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
Write-Host "=== Offset 510..511 (MBR signature, expect 55 AA) ==="
'{0:X2} {1:X2}' -f $bytes[510], $bytes[511]
Write-Host "=== Last 32 bytes (end) ==="
($bytes[($bytes.Length-32)..($bytes.Length-1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '