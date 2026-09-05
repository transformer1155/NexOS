# Run the e2e test directly under Windows Python (no WSL, no swap issue)
$p = Start-Process -FilePath 'python3' -ArgumentList 'D:\MyOS\Bootloader\tools\test_e2e_stdlib.py' -PassThru -NoNewWindow -RedirectStandardOutput 'D:\MyOS\Bootloader\tools\_e2e.txt' -RedirectStandardError 'D:\MyOS\Bootloader\tools\_e2e_err.txt'
Write-Host ('E2E_PID=' + $p.Id)
$p.WaitForExit(220000)
if (-not $p.HasExited) {
    Write-Host 'TIMEOUT - killing'
    $p.Kill()
}
Write-Host '=== STDOUT ==='
Get-Content 'D:\MyOS\Bootloader\tools\_e2e.txt'
Write-Host '=== STDERR ==='
Get-Content 'D:\MyOS\Bootloader\tools\_e2e_err.txt'
