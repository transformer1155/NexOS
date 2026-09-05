Select-String -Path 'D:\MyOS\Bootloader\kernel.cpp' -Pattern 'g_term_serial|put_char|render_all|GUI] render|serial_puts' | ForEach-Object { Write-Host ($_.LineNumber.ToString() + ': ' + $_.Line) }
