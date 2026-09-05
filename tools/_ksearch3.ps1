Select-String -Path 'D:\MyOS\Bootloader\kernel.cpp' -Pattern 'g_term_serial|render_all|void.*put_char|serial' | ForEach-Object { Write-Host ($_.LineNumber.ToString() + ': ' + $_.Line) }
