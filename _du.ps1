$top = "C:\"
$dirs = Get-ChildItem $top -Force -ErrorAction SilentlyContinue | Where-Object { $_.PSIsContainer }
$rows = @()
foreach ($d in $dirs) {
    try {
        $sz = (Get-ChildItem $d.FullName -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    } catch { $sz = 0 }
    $rows += [PSCustomObject]@{ Name = $d.Name; GB = [math]::Round($sz/1GB,2) }
}
$rows | Sort-Object GB -Descending | Format-Table -AutoSize | Out-String -Width 120
Write-Host "C free MB:" ([math]::Round((Get-PSDrive C).Free/1MB,0))
