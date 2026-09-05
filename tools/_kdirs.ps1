Get-ChildItem 'D:\MyOS\Bootloader\kernel' -Recurse | ForEach-Object { Write-Host $_.FullName }
